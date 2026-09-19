#ifndef WIN32

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "PseudoUserTerminal.hpp"
#include "TestHeaders.hpp"

namespace et {
namespace {

using namespace std::chrono_literals;

const string kShellReadyMarker = "ET_PSEUDO_HUP_IGNORING_SHELL_READY";

class IgnoringHupShell {
 public:
  IgnoringHupShell() {
    string tmpPath = GetTempDirectory() + "et_test_hup_XXXXXXXX";
    directory = string(mkdtemp(&tmpPath[0]));
    if (directory.empty()) {
      throw runtime_error("Could not create isolated shell directory");
    }

    path = directory + "/ignore-hup-shell";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0700);
    if (fd == -1) {
      throw runtime_error("Could not create isolated shell");
    }

    const string script =
        "#!/bin/sh\n"
        "trap '' HUP\n"
        "printf '" +
        kShellReadyMarker +
        "\\n'\n"
        "exec /bin/sh -c 'trap \"\" HUP; while :; do "
        "sleep 10; done'\n";
    size_t offset = 0;
    while (offset < script.size()) {
      const ssize_t written =
          ::write(fd, script.data() + offset, script.size() - offset);
      if (written <= 0) {
        ::close(fd);
        throw runtime_error("Could not write isolated shell");
      }
      offset += static_cast<size_t>(written);
    }
    if (::close(fd) == -1) {
      throw runtime_error("Could not close isolated shell");
    }
  }

  ~IgnoringHupShell() {
    if (!path.empty()) {
      ::unlink(path.c_str());
    }
    if (!directory.empty()) {
      ::rmdir(directory.c_str());
    }
  }

  const string& getPath() const { return path; }

 private:
  string directory;
  string path;
};

class ScopedShellEnvironment {
 public:
  explicit ScopedShellEnvironment(const string& shell) {
    const char* previous = ::getenv("SHELL");
    if (previous != nullptr) {
      previousShell = previous;
    }
    if (::setenv("SHELL", shell.c_str(), 1) == -1) {
      throw runtime_error("Could not set isolated SHELL");
    }
  }

  ~ScopedShellEnvironment() {
    if (previousShell.has_value()) {
      ::setenv("SHELL", previousShell.value().c_str(), 1);
    } else {
      ::unsetenv("SHELL");
    }
  }

 private:
  optional<string> previousShell;
};

bool waitForOutput(int fd, const string& marker,
                   std::chrono::milliseconds timeout) {
  string output;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    char buffer[256];
    const ssize_t readCount = ::read(fd, buffer, sizeof(buffer));
    if (readCount > 0) {
      output.append(buffer, static_cast<size_t>(readCount));
      if (output.find(marker) != string::npos) {
        return true;
      }
    } else if (readCount == -1 && errno != EAGAIN && errno != EWOULDBLOCK &&
               errno != EINTR) {
      return false;
    }
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

void killOwnedChildForTest(PseudoUserTerminal& terminal) {
  const pid_t childPid = terminal.getPid();
  if (childPid <= 0) {
    return;
  }

  int status = 0;
  pid_t waitResult;
  do {
    waitResult = ::waitpid(childPid, &status, WNOHANG);
  } while (waitResult == -1 && errno == EINTR);
  if (waitResult != 0) {
    return;
  }

  // The test owns this child and its forkpty-created group.  Recheck both
  // identities before using the negative-pid group operation.
  if (::getpgid(childPid) == childPid) {
    ::kill(-childPid, SIGKILL);
    return;
  }

  // If the group identity disappeared, recheck that the direct child is still
  // unreaped before using its PID for the bounded test cleanup.
  do {
    waitResult = ::waitpid(childPid, &status, WNOHANG);
  } while (waitResult == -1 && errno == EINTR);
  if (waitResult == 0) {
    ::kill(childPid, SIGKILL);
  }
}

void reapOwnedChildForTest(pid_t childPid) {
  if (childPid <= 0) {
    return;
  }

  int status = 0;
  pid_t waitResult;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    do {
      waitResult = ::waitpid(childPid, &status, WNOHANG);
    } while (waitResult == -1 && errno == EINTR);
    if (waitResult == childPid || (waitResult == -1 && errno == ECHILD)) {
      return;
    }
    if (waitResult == -1) {
      return;
    }
    std::this_thread::sleep_for(10ms);
  }

  // The child is still owned by this test.  A final direct SIGKILL keeps
  // cleanup bounded even if the group signal raced with process exit.
  ::kill(childPid, SIGKILL);
  do {
    waitResult = ::waitpid(childPid, &status, 0);
  } while (waitResult == -1 && errno == EINTR);
}

class ScopedPseudoUserTerminal {
 public:
  ~ScopedPseudoUserTerminal() {
    if (!started) {
      return;
    }
    killOwnedChildForTest(terminal);
    reapOwnedChildForTest(terminal.getPid());
    terminal.cleanup();
    if (terminal.getFd() >= 0) {
      ::close(terminal.getFd());
    }
  }

  PseudoUserTerminal terminal;
  bool started = false;
};

}  // namespace

TEST_CASE("PseudoUserTerminal bounds HUP-ignoring shell termination",
          "[PseudoUserTerminal][integration]") {
  IgnoringHupShell shell;
  ScopedShellEnvironment shellEnvironment(shell.getPath());
  ScopedPseudoUserTerminal scopedTerminal;
  PseudoUserTerminal& terminal = scopedTerminal.terminal;

  const int masterFd = terminal.setup(-1);
  scopedTerminal.started = true;
  REQUIRE(masterFd >= 0);
  REQUIRE(waitForOutput(masterFd, kShellReadyMarker, 5s));

  atomic<bool> terminateReturned(false);
  thread terminateThread([&terminal, &terminateReturned]() {
    terminal.terminate();
    terminateReturned = true;
  });

  const auto terminateDeadline = std::chrono::steady_clock::now() + 2s;
  while (!terminateReturned &&
         std::chrono::steady_clock::now() < terminateDeadline) {
    std::this_thread::sleep_for(10ms);
  }

  const bool terminateCompletedBeforeCleanup = terminateReturned.load();
  if (!terminateCompletedBeforeCleanup) {
    killOwnedChildForTest(terminal);
  }
  if (terminateThread.joinable()) {
    terminateThread.join();
  }
  REQUIRE(terminateCompletedBeforeCleanup);

  atomic<bool> sessionEndReturned(false);
  thread sessionEndThread([&terminal, &sessionEndReturned]() {
    terminal.handleSessionEnd();
    sessionEndReturned = true;
  });

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!sessionEndReturned && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }

  const bool completedBeforeCleanup = sessionEndReturned.load();

  if (!completedBeforeCleanup) {
    // Keep a pre-fix run bounded while still making the failure observable.
    killOwnedChildForTest(terminal);
  }
  if (sessionEndThread.joinable()) {
    sessionEndThread.join();
  }

  REQUIRE(completedBeforeCleanup);
}

}  // namespace et

#endif
