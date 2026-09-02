#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#include <vector>

#include "ControlMode.hpp"
#include "HtmTestHelpers.hpp"
#include "TestHeaders.hpp"

#ifndef WIN32
#if defined(__APPLE__) || defined(__NetBSD__)
#include <util.h>
#elif defined(__FreeBSD__)
#include <libutil.h>
#else
#include <pty.h>
#endif
#include <termios.h>

using namespace et;
using namespace et::htmtest;

namespace {

string htmBinDir() {
  if (const char* env = getenv("ET_BUILD_DIR")) {
    return env;
  }
  if (access("./htm", X_OK) == 0 && access("./htmd", X_OK) == 0) {
    return ".";
  }
  if (access("../build/htm", X_OK) == 0 && access("../build/htmd", X_OK) == 0) {
    return "../build";
  }
  return "build";
}

string htmPath() { return htmBinDir() + "/htm"; }
string htmdPath() { return htmBinDir() + "/htmd"; }
bool executable(const string& path) { return access(path.c_str(), X_OK) == 0; }
uid_t selfUid() { return getuid(); }
string ipcPath() {
  return string(_PATH_TMP) + "htm." + GetHtmIpcUser() + ".ipc";
}
bool htmdRunning() {
  string cmd =
      string("pgrep -x -U ") + to_string(selfUid()) + " htmd >/dev/null 2>&1";
  return system(cmd.c_str()) == 0;
}
void killHtmd() {
  string cmd =
      string("pkill -x -U ") + to_string(selfUid()) + " htmd >/dev/null 2>&1";
  system(cmd.c_str());
  waitUntil([]() { return !htmdRunning(); }, 3000);
}
void killHtmClients() {
  string cmd =
      string("pkill -x -U ") + to_string(selfUid()) + " htm >/dev/null 2>&1";
  system(cmd.c_str());
}

class ControlPty {
 public:
  explicit ControlPty(bool killOther = true) {
    REQUIRE(executable(htmPath()));
    REQUIRE(executable(htmdPath()));
    int slave = -1;
    REQUIRE(openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
    pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
      ::close(master);
      setsid();
      ioctl(slave, TIOCSCTTY, 0);
      dup2(slave, STDIN_FILENO);
      dup2(slave, STDOUT_FILENO);
      dup2(slave, STDERR_FILENO);
      if (slave > STDERR_FILENO) {
        ::close(slave);
      }
      string dir;
      char resolved[PATH_MAX];
      if (realpath(htmBinDir().c_str(), resolved)) {
        dir = resolved;
      } else {
        dir = htmBinDir();
      }
      string path = dir + ":" + (getenv("PATH") ? getenv("PATH") : "");
      setenv("PATH", path.c_str(), 1);
      string bin = dir + "/htm";
      if (killOther) {
        execl(bin.c_str(), "htm", "-x", (char*)nullptr);
      } else {
        execl(bin.c_str(), "htm", (char*)nullptr);
      }
      _exit(127);
    }
    ::close(slave);
    int flags = fcntl(master, F_GETFL, 0);
    REQUIRE(fcntl(master, F_SETFL, flags | O_NONBLOCK) == 0);
  }

  ~ControlPty() { closeSession(); }

  void closeSession() {
    if (master >= 0) {
      ::close(master);
      master = -1;
    }
    if (pid > 0) {
      ::kill(pid, SIGTERM);
      int status = 0;
      waitUntil([&]() { return waitpid(pid, &status, WNOHANG) == pid; }, 4000);
      if (waitpid(pid, &status, WNOHANG) != pid) {
        ::kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
      }
      pid = -1;
    }
  }

  void writeRaw(const string& data) {
    size_t off = 0;
    while (off < data.size()) {
      ssize_t n = ::write(master, data.data() + off, data.size() - off);
      if (n > 0) {
        off += size_t(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
        pump(5);
        continue;
      }
      FAIL("write to HTM PTY failed");
    }
  }

  void sendCommand(const string& line) { writeRaw(line + "\n"); }

  bool pump(int timeoutMs = 200) {
    const auto start = std::chrono::steady_clock::now();
    bool got = false;
    while (true) {
      char tmp[4096];
      ssize_t n = ::read(master, tmp, sizeof(tmp));
      if (n > 0) {
        got = true;
        incoming.append(tmp, size_t(n));
        lines = splitLines(incoming);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        if (elapsed >= timeoutMs) {
          return got;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      if (n == 0) {
        eof = true;
        return got;
      }
      if (n < 0 && errno == EINTR) {
        continue;
      }
      return got;
    }
  }

  bool waitFor(const std::function<bool()>& pred, int timeoutMs = 8000) {
    const auto start = std::chrono::steady_clock::now();
    while (!pred()) {
      pump(50);
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
      if (elapsed > timeoutMs) {
        return false;
      }
    }
    return true;
  }

  bool waitAttached() {
    return waitFor(
        [&]() { return incoming.find("%session-changed") != string::npos; });
  }

  string incoming;
  vector<string> lines;
  bool eof = false;
  pid_t pid = -1;
  int master = -1;
};

struct IsolatedHtmd {
  string lockDir;
  IsolatedHtmd() {
    lockDir =
        string(_PATH_TMP) + "htm.e2e." + to_string(selfUid()) + ".lockdir";
    const auto start = std::chrono::steady_clock::now();
    while (mkdir(lockDir.c_str(), 0700) != 0) {
      if (errno != EEXIST) {
        FAIL("mkdir HTM e2e lock failed: errno=" << errno);
      }
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
      if (elapsed > 120000) {
        FAIL("Timed out waiting for HTM e2e lock");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    killHtmClients();
    killHtmd();
    ::remove(ipcPath().c_str());
  }
  ~IsolatedHtmd() {
    killHtmClients();
    killHtmd();
    ::remove(ipcPath().c_str());
    if (!lockDir.empty()) {
      rmdir(lockDir.c_str());
    }
  }
};

}  // namespace

TEST_CASE("Control-mode PTY: DCS, session-changed, commands, shutdown",
          "[Htm][e2e][pty]") {
  if (!executable(htmPath()) || !executable(htmdPath())) {
    SKIP("htm/htmd are not built");
  }
  IsolatedHtmd isolate;
  ControlPty pty;
  REQUIRE(pty.waitFor(
      [&]() { return pty.incoming.find("1000p") != string::npos; }, 8000));
  REQUIRE(pty.waitAttached());
  pty.sendCommand("display-message -p '#{version}'");
  REQUIRE(pty.waitFor(
      [&]() { return pty.incoming.find(HTM_TMUX_VERSION) != string::npos; }));
  pty.sendCommand("new-window");
  REQUIRE(pty.waitFor(
      [&]() { return pty.incoming.find("%window-add") != string::npos; }));
  pty.sendCommand("split-window -h");
  REQUIRE(pty.waitFor(
      [&]() { return pty.incoming.find("%layout-change") != string::npos; }));
  pty.sendCommand("kill-server");
  REQUIRE(pty.waitFor([&]() { return !htmdRunning() || pty.eof; }, 8000));
}

TEST_CASE("Control-mode PTY: detach leaves htmd running", "[Htm][e2e][pty]") {
  if (!executable(htmPath()) || !executable(htmdPath())) {
    SKIP("htm/htmd are not built");
  }
  IsolatedHtmd isolate;
  {
    ControlPty pty;
    REQUIRE(pty.waitAttached());
    pty.sendCommand("");
    REQUIRE(pty.waitFor(
        [&]() { return pty.incoming.find("%exit") != string::npos; }));
  }
  REQUIRE(htmdRunning());
  ControlPty reconnect(false);
  REQUIRE(reconnect.waitAttached());
  reconnect.sendCommand("kill-server");
  reconnect.waitFor([&]() { return !htmdRunning(); }, 8000);
}

TEST_CASE("Control-mode PTY: SIGKILL of htm lets htmd accept a new client",
          "[Htm][e2e][pty]") {
  if (!executable(htmPath()) || !executable(htmdPath())) {
    SKIP("htm/htmd are not built");
  }
  IsolatedHtmd isolate;
  pid_t firstPid = -1;
  {
    ControlPty pty;
    REQUIRE(pty.waitAttached());
    firstPid = pty.pid;
    ::kill(firstPid, SIGKILL);
    pty.pid = -1;
    pty.closeSession();
  }
  REQUIRE(htmdRunning());
  ControlPty reconnect(false);
  REQUIRE(reconnect.waitAttached());
  reconnect.sendCommand("kill-server");
  reconnect.waitFor([&]() { return !htmdRunning(); }, 8000);
}

TEST_CASE("Ghostty GUI: control-mode attach is opt-in until Ghostty speaks -CC",
          "[Htm][.ghostty][e2e][gui]") {
  SKIP(
      "Ghostty control-mode / tmux -CC integration is not required in default "
      "CI");
}

#endif
