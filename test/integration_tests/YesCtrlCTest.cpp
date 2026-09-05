#include "TestHeaders.hpp"
#include "WriteBuffer.hpp"

#ifndef WIN32

namespace {

class PtyShell {
 public:
  PtyShell() {
    struct termios term;
    memset(&term, 0, sizeof(term));
    cfmakeraw(&term);
    term.c_lflag |= ISIG;
    term.c_cc[VINTR] = '\x03';
    term.c_cc[VQUIT] = '\x1c';
    term.c_cc[VSUSP] = '\x1a';

    pid = forkpty(&masterFd, nullptr, &term, nullptr);
    REQUIRE(pid >= 0);
    if (pid == 0) {
      setenv("PS1", prompt, 1);
      execl("/bin/sh", "sh", "-i", (char*)nullptr);
      _exit(127);
    }
    int flags = fcntl(masterFd, F_GETFL, 0);
    REQUIRE(flags >= 0);
    REQUIRE(fcntl(masterFd, F_SETFL, flags | O_NONBLOCK) == 0);
  }

  ~PtyShell() {
    if (masterFd >= 0) {
      ::close(masterFd);
    }
    stop();
  }

  void stop() {
    if (pid > 0) {
      // forkpty makes the child a process-group leader, so this also stops a
      // `yes` child if an assertion fails before Ctrl+C reaches it.
      ::kill(-pid, SIGTERM);
      ::kill(pid, SIGTERM);
      for (int i = 0; i < 100; ++i) {
        if (waitpid(pid, nullptr, WNOHANG) == pid) {
          pid = -1;
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      ::kill(-pid, SIGKILL);
      ::kill(pid, SIGKILL);
      waitpid(pid, nullptr, 0);
      pid = -1;
    }
  }

  void writeAll(const string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
      ssize_t written =
          ::write(masterFd, data.data() + offset, data.size() - offset);
      if (written > 0) {
        offset += written;
      } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        FAIL("Failed to write to test pty: " << strerror(errno));
      }
    }
  }

  string readFor(std::chrono::milliseconds timeout) {
    string result;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buf[16 * 1024];
    while (std::chrono::steady_clock::now() < deadline) {
      fd_set rfd;
      FD_ZERO(&rfd);
      FD_SET(masterFd, &rfd);
      timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = 10000;
      int ready = select(masterFd + 1, &rfd, nullptr, nullptr, &tv);
      if (ready > 0) {
        ssize_t count = ::read(masterFd, buf, sizeof(buf));
        if (count > 0) {
          result.append(buf, count);
          continue;
        }
        if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
          break;
        }
      }
    }
    return result;
  }

  string readUntil(const string& marker, std::chrono::milliseconds timeout) {
    string result;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (result.find(marker) == string::npos &&
           std::chrono::steady_clock::now() < deadline) {
      result += readFor(std::chrono::milliseconds(20));
    }
    return result;
  }

  static constexpr const char* prompt = "ET_YES_PROMPT> ";

 private:
  int masterFd = -1;
  pid_t pid = -1;
};

}  // namespace

TEST_CASE("Ctrl+C flushes real yes output and reveals the shell prompt",
          "[YesCtrlC][integration]") {
  PtyShell shell;
  REQUIRE(shell.readUntil(PtyShell::prompt, std::chrono::seconds(3))
              .find(PtyShell::prompt) != string::npos);

  shell.writeAll("yes ET_YES\n");

  // Read only enough real `yes` output to exceed the flush threshold. This
  // exercises an endless producer without a long soak or meaningful CPU use.
  et::WriteBuffer pendingOutput;
  const auto fillDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!pendingOutput.shouldFlushOnInterrupt() &&
         std::chrono::steady_clock::now() < fillDeadline) {
    pendingOutput.enqueue(shell.readFor(std::chrono::milliseconds(20)));
  }
  REQUIRE(pendingOutput.shouldFlushOnInterrupt());

  const string ctrlC(1, '\x03');
  REQUIRE(et::WriteBuffer::containsInterruptByte(ctrlC));
  REQUIRE(pendingOutput.flushIfLarge() >= et::WriteBuffer::FLUSH_THRESHOLD);

  // Match TerminalServer's order: discard bytes already readable from the
  // PTY, then forward Ctrl+C. Output produced after this point includes the
  // shell prompt and must not be discarded.
  shell.readFor(std::chrono::milliseconds(1));
  const auto interruptTime = std::chrono::steady_clock::now();
  shell.writeAll(ctrlC);
  const string afterInterrupt =
      shell.readUntil(PtyShell::prompt, std::chrono::seconds(2));
  const auto interruptLatency =
      std::chrono::steady_clock::now() - interruptTime;

  REQUIRE(afterInterrupt.find(PtyShell::prompt) != string::npos);
  REQUIRE(interruptLatency < std::chrono::seconds(2));

  shell.stop();
}

#endif
