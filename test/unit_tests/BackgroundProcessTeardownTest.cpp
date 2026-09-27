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
  bool openPidPipe() { return ::pipe(grandchildPidPipe) == 0; }

  ~DescendantHoldsSlaveTerminal() override {
    reapGrandchild();
    closePidPipe();
  }

  void runTerminal() override {
    ::close(grandchildPidPipe[0]);
    grandchildPidPipe[0] = -1;

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
      ::close(grandchildPidPipe[1]);
      ::signal(SIGHUP, SIG_IGN);
      const int held = ::open(slaveName, O_RDWR);
      if (held < 0) {
        _exit(127);
      }
      // Keep held open for the lifetime of this process.
      execl("/bin/sleep", "sleep", "60", static_cast<char*>(nullptr));
      _exit(127);
    }

    const ssize_t written =
        ::write(grandchildPidPipe[1], &grandchild, sizeof(grandchild));
    ::close(grandchildPidPipe[1]);
    grandchildPidPipe[1] = -1;
    if (written != static_cast<ssize_t>(sizeof(grandchild))) {
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

  void closePidPipe() {
    for (int& fd : grandchildPidPipe) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }
  }

  pid_t knownGrandchild = -1;
  int grandchildPidPipe[2] = {-1, -1};
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

  // Master must not be at EOF: a non-blocking read is EAGAIN while the
  // grandchild still holds the slave (and the holder is still alive).
  char probe = 0;
  const ssize_t n = ::read(masterFd, &probe, 1);
  REQUIRE(n != 0);
  if (n < 0) {
    REQUIRE((errno == EAGAIN || errno == EWOULDBLOCK));
  }
  REQUIRE(::kill(term.knownGrandchild, 0) == 0);

  REQUIRE(term.handleSessionEnd() == 42);

  term.reapGrandchild();
  term.cleanup();
}
}  // namespace et
#endif
