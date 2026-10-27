#include "TestHeaders.hpp"

#ifndef WIN32
#include <chrono>
#include <stdexcept>
#include <thread>

#include "PseudoUserTerminal.hpp"

namespace et {
namespace {

using namespace std::chrono_literals;

const string kSshTtyMarkerPrefix = "ET_SSH_TTY_MARKER:";

class ReportingShell {
 public:
  ReportingShell() {
    string tmpPath = GetTempDirectory() + "et_test_ssh_tty_XXXXXXXX";
    directory = string(mkdtemp(&tmpPath[0]));
    if (directory.empty()) {
      throw runtime_error("Could not create isolated shell directory");
    }

    path = directory + "/report-ssh-tty-shell";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0700);
    if (fd == -1) {
      throw runtime_error("Could not create isolated shell");
    }

    // Print SSH_TTY as seen by the shell after
    // PseudoUserTerminal::runTerminal() has prepared the environment, then exit
    // so the test can reap quickly.
    const string script =
        "#!/bin/sh\n"
        "printf '" +
        kSshTtyMarkerPrefix + "%s\\n' \"${SSH_TTY-}\"\n";
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

  ~ReportingShell() {
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

// Ensure a parent SSH_TTY cannot masquerade as production setenv success.
class ScopedUnsetSshTty {
 public:
  ScopedUnsetSshTty() {
    const char* previous = ::getenv("SSH_TTY");
    if (previous != nullptr) {
      previousSshTty = previous;
    }
    ::unsetenv("SSH_TTY");
  }

  ~ScopedUnsetSshTty() {
    if (previousSshTty.has_value()) {
      ::setenv("SSH_TTY", previousSshTty.value().c_str(), 1);
    } else {
      ::unsetenv("SSH_TTY");
    }
  }

 private:
  optional<string> previousSshTty;
};

string waitForMarkerLine(int fd, const string& prefix,
                         std::chrono::milliseconds timeout) {
  string output;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    char buffer[256];
    const ssize_t readCount = ::read(fd, buffer, sizeof(buffer));
    if (readCount > 0) {
      output.append(buffer, static_cast<size_t>(readCount));
      const size_t start = output.find(prefix);
      if (start != string::npos) {
        const size_t end = output.find('\n', start);
        if (end != string::npos) {
          string value = output.substr(start + prefix.size(),
                                       end - (start + prefix.size()));
          // forkpty masters often translate NL to CRLF.
          if (!value.empty() && value.back() == '\r') {
            value.pop_back();
          }
          return value;
        }
      }
    } else if (readCount == -1 && errno != EAGAIN && errno != EWOULDBLOCK &&
               errno != EINTR) {
      return "";
    }
    std::this_thread::sleep_for(10ms);
  }
  return "";
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

  if (::getpgid(childPid) == childPid) {
    ::kill(-childPid, SIGKILL);
    return;
  }

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

TEST_CASE("PseudoUserTerminal sets SSH_TTY for allocated PTY",
          "[PseudoUserTerminal][SSH_TTY]") {
  // Drive PseudoUserTerminal::setup() -> forkpty child -> runTerminal(),
  // which must setenv("SSH_TTY", ttyname(STDIN_FILENO), 1) before exec.
  // A custom SHELL prints the value so removing that setenv fails this test.
  ReportingShell shell;
  ScopedShellEnvironment shellEnvironment(shell.getPath());
  ScopedUnsetSshTty unsetSshTty;
  ScopedPseudoUserTerminal scopedTerminal;
  PseudoUserTerminal& terminal = scopedTerminal.terminal;

  const int masterFd = terminal.setup(-1);
  scopedTerminal.started = true;
  REQUIRE(masterFd >= 0);

  const string sshTty = waitForMarkerLine(masterFd, kSshTtyMarkerPrefix, 5s);
  // Empty means runTerminal() did not setenv("SSH_TTY", ...) before exec.
  REQUIRE_FALSE(sshTty.empty());
  REQUIRE(sshTty.rfind("/dev/", 0) == 0);
}

}  // namespace et

#else

TEST_CASE("PseudoUserTerminal sets SSH_TTY for allocated PTY",
          "[PseudoUserTerminal][SSH_TTY]") {
  SKIP("SSH_TTY is only set for POSIX PTY sessions");
}

#endif
