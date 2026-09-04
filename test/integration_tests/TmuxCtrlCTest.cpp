#include "TestHeaders.hpp"
#include "TmuxCcFilter.hpp"
#include "WriteBuffer.hpp"

#ifndef WIN32

namespace {

// True if a `tmux` binary is on PATH. Both tests below SKIP when it is not,
// so the suite still passes in minimal environments.
bool tmuxAvailable() {
  return ::system("command -v tmux >/dev/null 2>&1") == 0;
}

// Forks a real PTY that runs a `tmux` client directly (no login shell), on
// a private, randomly-named control socket so this never touches (or is
// confused with) a real user's tmux server.
class TmuxPtyShell {
 public:
  explicit TmuxPtyShell(bool controlMode) {
    socketName = "et_test_tmux_" + et::genRandomAlphaNum(12);

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
      setenv("TERM", "xterm-256color", 1);
      if (controlMode) {
        execlp("tmux", "tmux", "-L", socketName.c_str(), "-f", "/dev/null",
               "-CC", "new-session", "-x", "80", "-y", "24", "/bin/sh -i",
               (char*)nullptr);
      } else {
        execlp("tmux", "tmux", "-L", socketName.c_str(), "-f", "/dev/null",
               "new-session", "-x", "80", "-y", "24", "/bin/sh -i",
               (char*)nullptr);
      }
      _exit(127);
    }
    int flags = fcntl(masterFd, F_GETFL, 0);
    REQUIRE(flags >= 0);
    REQUIRE(fcntl(masterFd, F_SETFL, flags | O_NONBLOCK) == 0);
  }

  ~TmuxPtyShell() {
    if (masterFd >= 0) {
      ::close(masterFd);
    }
    stop();
    // The tmux server outlives the client we forked above; tear it down
    // explicitly so no stray server survives the test.
    string killCommand =
        "tmux -L " + socketName + " kill-server >/dev/null 2>&1";
    int rc = ::system(killCommand.c_str());
    (void)rc;
    // tmux does not always unlink its socket by the time kill-server
    // returns; remove it so /tmp does not accumulate test sockets.
    const char* tmpdir = getenv("TMUX_TMPDIR");
    string socketDir =
        string(tmpdir ? tmpdir : "/tmp") + "/tmux-" + to_string(getuid());
    ::remove((socketDir + "/" + socketName).c_str());
  }

  void stop() {
    if (pid > 0) {
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

  static constexpr const char* prompt = "ET_TMUX_PROMPT> ";

 private:
  int masterFd = -1;
  pid_t pid = -1;
  string socketName;
};

}  // namespace

TEST_CASE("Ctrl+C reaches yes through a plain tmux pane and flushes the spam",
          "[TmuxCtrlC][integration]") {
  if (!tmuxAvailable()) {
    SKIP("tmux binary not found in PATH");
  }

  TmuxPtyShell shell(/*controlMode=*/false);
  REQUIRE(shell.readUntil(TmuxPtyShell::prompt, std::chrono::seconds(5))
              .find(TmuxPtyShell::prompt) != string::npos);

  shell.writeAll("yes ET_YES\n");

  // Mirror TerminalServer: buffer real `yes` output from the pty until it
  // would trigger a flush, then confirm the interrupt byte is detected and
  // dropping the backlog actually frees a substantial amount of data.
  et::WriteBuffer pendingOutput;
  const auto fillDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!pendingOutput.shouldFlushOnInterrupt() &&
         std::chrono::steady_clock::now() < fillDeadline) {
    pendingOutput.enqueue(shell.readFor(std::chrono::milliseconds(20)));
  }
  REQUIRE(pendingOutput.shouldFlushOnInterrupt());

  const string ctrlC(1, '\x03');
  REQUIRE(et::WriteBuffer::containsInterruptByte(ctrlC));
  REQUIRE(pendingOutput.flushIfLarge() >= et::WriteBuffer::FLUSH_THRESHOLD);

  // Discard bytes already readable (as TerminalServer does before forwarding
  // the interrupt), then send Ctrl+C through the real tmux pane to the real
  // `yes` process and confirm the shell prompt comes back promptly.
  shell.readFor(std::chrono::milliseconds(1));
  const auto interruptTime = std::chrono::steady_clock::now();
  shell.writeAll(ctrlC);
  const string afterInterrupt =
      shell.readUntil(TmuxPtyShell::prompt, std::chrono::seconds(5));
  const auto interruptLatency =
      std::chrono::steady_clock::now() - interruptTime;

  REQUIRE(afterInterrupt.find(TmuxPtyShell::prompt) != string::npos);
  REQUIRE(interruptLatency < std::chrono::seconds(5));

  shell.stop();
}

TEST_CASE(
    "tmux -CC control-mode Ctrl+C keeps notifications and drops %output "
    "flood",
    "[TmuxCtrlC][TmuxCc][integration]") {
  if (!tmuxAvailable()) {
    SKIP("tmux binary not found in PATH");
  }

  // No GUI is attached; a real `tmux -CC` control-mode session is driven
  // directly with the same text commands a GUI client (e.g. iTerm2) would
  // send over the control channel. This exercises TmuxCcFilter and
  // tmuxCcContainsInterruptCommand against a genuine tmux control-mode byte
  // stream, not a hand-written fixture.
  TmuxPtyShell shell(/*controlMode=*/true);

  // The initial %begin/%end block from the implicit `list-sessions` that
  // `-CC new-session` issues on attach.
  string startup = shell.readUntil("%end", std::chrono::seconds(5));
  REQUIRE(startup.find("%begin") != string::npos);

  // Start a real, endless producer in the pane, using the control protocol
  // (a real GUI client would send exactly this command).
  shell.writeAll("send-keys \"yes ET_CC_YES\" Enter\n");

  et::WriteBuffer pendingOutput;
  const auto fillDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!pendingOutput.shouldFlushOnInterrupt() &&
         std::chrono::steady_clock::now() < fillDeadline) {
    pendingOutput.enqueue(shell.readFor(std::chrono::milliseconds(20)));
  }
  REQUIRE(pendingOutput.shouldFlushOnInterrupt());

  // The GUI does not send a raw 0x03; it issues this control command. The
  // plain byte scan must miss it, and the new tmux-aware scan must catch it.
  const string interruptCommand = "send-keys -H 3\n";
  REQUIRE_FALSE(et::WriteBuffer::containsInterruptByte(interruptCommand));
  REQUIRE(et::tmuxCcContainsInterruptCommand(interruptCommand));

  size_t dropped = pendingOutput.flushIfLarge();
  REQUIRE(dropped >= et::WriteBuffer::FLUSH_THRESHOLD);
  // Anything kept across the flush must not be droppable pane output.
  size_t keptCount = 0;
  {
    size_t count = 0;
    const char* data = pendingOutput.peekData(&count);
    if (data != nullptr) {
      et::TmuxCcFilterResult recheck = et::filterTmuxCc(string(data, count));
      keptCount = recheck.dropped;
    }
  }
  REQUIRE(keptCount == 0);

  // Forward the interrupt for real and confirm the pane's shell becomes
  // responsive again (a fresh command's output shows up in `%output`).
  shell.writeAll(interruptCommand);
  shell.writeAll("send-keys \"echo TMUX_CC_DONE\" Enter\n");
  const string afterInterrupt =
      shell.readUntil("TMUX_CC_DONE", std::chrono::seconds(5));
  REQUIRE(afterInterrupt.find("TMUX_CC_DONE") != string::npos);

  shell.stop();
}

#endif
