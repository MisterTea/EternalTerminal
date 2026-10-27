#include "TestHeaders.hpp"

#ifndef WIN32
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "PseudoUserTerminal.hpp"

namespace et {
class TestPseudoUserTerminal : public PseudoUserTerminal {
 public:
  void setChildReaped(bool reaped) { childReaped = reaped; }

  void recordReapedWaitStatusForTest(int status) {
    recordReapedWaitStatus(status);
  }

  int exitStatusForTest() const { return exitStatus; }
};

class ImmediateExitTerminal : public PseudoUserTerminal {
 public:
  void runTerminal() override { _exit(42); }
};

TEST_CASE("BackgroundProcessTeardown", "[BackgroundProcessTeardown]") {
  TestPseudoUserTerminal term;
  REQUIRE_FALSE(term.sessionHasEnded());
  term.setChildReaped(true);
  REQUIRE(term.sessionHasEnded());
}

TEST_CASE("sessionHasEnded preserves exit status for handleSessionEnd",
          "[BackgroundProcessTeardown]") {
  ImmediateExitTerminal term;
  int routerFds[2] = {-1, -1};
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, routerFds) == 0);

  const int masterFd = term.setup(routerFds[0]);
  REQUIRE(masterFd >= 0);
  ::close(routerFds[0]);
  ::close(routerFds[1]);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!term.sessionHasEnded() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(term.sessionHasEnded());
  REQUIRE(term.handleSessionEnd() == 42);
  term.cleanup();
}

TEST_CASE("recordReapedWaitStatus caches OpenSSH-style exit status",
          "[BackgroundProcessTeardown]") {
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    _exit(42);
  }
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 42);

  TestPseudoUserTerminal term;
  term.recordReapedWaitStatusForTest(status);
  REQUIRE(term.sessionHasEnded());
  REQUIRE(term.exitStatusForTest() == 42);
  REQUIRE(term.handleSessionEnd() == 42);
}

TEST_CASE("recordReapedWaitStatus caches OpenSSH-style signal status",
          "[BackgroundProcessTeardown]") {
  // Synthesize a wait(2) status: sending SIGTERM to a forked child trips
  // Catch's process-wide fatal signal handler.
  const int status = SIGTERM;
  REQUIRE(WIFSIGNALED(status));
  REQUIRE(WTERMSIG(status) == SIGTERM);

  TestPseudoUserTerminal term;
  term.recordReapedWaitStatusForTest(status);
  REQUIRE(term.sessionHasEnded());
  REQUIRE(term.exitStatusForTest() == 128 + SIGTERM);
  REQUIRE(term.handleSessionEnd() == 128 + SIGTERM);
}

/**
 * Foreground forkpty child exits while a grandchild keeps the PTY slave open
 * (so the master does not EOF). sessionHasEnded() / handleSessionEnd() must
 * still observe the child's exit — the Issue #448 contract — rather than
 * waiting for master EOF.
 */
class DescendantHoldsSlaveTerminal : public PseudoUserTerminal {
 public:
  bool openPidPipe() {
    return ::pipe(grandchildPidPipe) == 0 && ::pipe(readyPipe) == 0;
  }

  ~DescendantHoldsSlaveTerminal() override {
    reapGrandchild();
    closePipes();
  }

  void runTerminal() override {
    // Capture the slave path before forking so the grandchild can reopen it
    // after we exit. On macOS, merely inheriting the slave fds is not enough
    // to keep the master from EOF once the session leader exits.
    char slaveName[128];
    if (::ttyname_r(STDIN_FILENO, slaveName, sizeof(slaveName)) != 0) {
      _exit(127);
    }

    const pid_t grandchild = ::fork();
    if (grandchild < 0) {
      _exit(127);
    }
    if (grandchild == 0) {
      ::close(grandchildPidPipe[0]);
      ::close(readyPipe[0]);
      // Leave the forkpty session/process group before the session leader
      // exits. Otherwise Darwin can reclaim the holder with the session.
      if (::setsid() < 0) {
        _exit(127);
      }
      ::signal(SIGHUP, SIG_IGN);
      const int held = ::open(slaveName, O_RDWR | O_NOCTTY);
      if (held < 0) {
        _exit(127);
      }

      // Publish pid only after setsid+open so the test sees a live holder.
      const pid_t self = ::getpid();
      const ssize_t written =
          ::write(grandchildPidPipe[1], &self, sizeof(self));
      ::close(grandchildPidPipe[1]);
      if (written != static_cast<ssize_t>(sizeof(self))) {
        _exit(127);
      }

      // Unblock the session-leader child only once we are fully detached and
      // holding the slave — otherwise it may exit while we are still in its
      // process group and Darwin will tear us down with the session.
      const char ready = 1;
      const ssize_t readyWritten = ::write(readyPipe[1], &ready, 1);
      ::close(readyPipe[1]);
      if (readyWritten != 1) {
        _exit(127);
      }

      // Keep held open for the lifetime of this process.
      execl("/bin/sleep", "sleep", "60", static_cast<char*>(nullptr));
      _exit(127);
    }

    // Session leader: wait for the holder to finish setup, then exit.
    ::close(grandchildPidPipe[0]);
    ::close(grandchildPidPipe[1]);
    ::close(readyPipe[1]);
    char ready = 0;
    const ssize_t n = ::read(readyPipe[0], &ready, 1);
    ::close(readyPipe[0]);
    if (n != 1) {
      ::kill(grandchild, SIGKILL);
      ::waitpid(grandchild, nullptr, 0);
      _exit(127);
    }
    _exit(42);
  }

  pid_t takeGrandchildPid() {
    if (grandchildPidPipe[1] >= 0) {
      ::close(grandchildPidPipe[1]);
      grandchildPidPipe[1] = -1;
    }
    if (grandchildPidPipe[0] < 0) {
      return -1;
    }
    pid_t grandchild = -1;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      const ssize_t n =
          ::read(grandchildPidPipe[0], &grandchild, sizeof(grandchild));
      if (n == static_cast<ssize_t>(sizeof(grandchild))) {
        ::close(grandchildPidPipe[0]);
        grandchildPidPipe[0] = -1;
        return grandchild;
      }
      if (n < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
    ::close(grandchildPidPipe[0]);
    grandchildPidPipe[0] = -1;
    return -1;
  }

  void reapGrandchild() {
    if (knownGrandchild <= 0) {
      return;
    }
    // After the forkpty child exits, init adopts the grandchild; we can only
    // kill it (not waitpid) so sleep does not leak.
    ::kill(knownGrandchild, SIGKILL);
    knownGrandchild = -1;
  }

  void closePipes() {
    for (int& fd : grandchildPidPipe) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }
    for (int& fd : readyPipe) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }
  }

  pid_t knownGrandchild = -1;
  int grandchildPidPipe[2] = {-1, -1};
  int readyPipe[2] = {-1, -1};
};

TEST_CASE("sessionHasEnded without master EOF while descendant holds PTY slave",
          "[BackgroundProcessTeardown]") {
  DescendantHoldsSlaveTerminal term;
  REQUIRE(term.openPidPipe());

  int routerFds[2] = {-1, -1};
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, routerFds) == 0);

  const int masterFd = term.setup(routerFds[0]);
  REQUIRE(masterFd >= 0);
  ::close(routerFds[0]);
  ::close(routerFds[1]);

  term.knownGrandchild = term.takeGrandchildPid();
  REQUIRE(term.knownGrandchild > 0);
  // Grandchild must still be alive (holding the slave).
  REQUIRE(::kill(term.knownGrandchild, 0) == 0);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!term.sessionHasEnded() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(term.sessionHasEnded());
  // Grandchild must still be alive: session end is driven by the foreground
  // child exit, not by waiting for unrelated descendants. (Do not probe the
  // master for EOF here — Darwin/FreeBSD EOF the master when the forkpty
  // session leader exits even if a descendant still holds a reopened slave.)
  REQUIRE(::kill(term.knownGrandchild, 0) == 0);

  REQUIRE(term.handleSessionEnd() == 42);

  term.reapGrandchild();
  term.cleanup();
}
}  // namespace et
#endif
