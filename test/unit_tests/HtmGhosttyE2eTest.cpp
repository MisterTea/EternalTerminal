#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "HtmHeaderCodes.hpp"
#include "HtmTestHelpers.hpp"
#include "TestHeaders.hpp"

// Ghostty e2e is opt-in ([.ghostty], hidden from default ./et-test and ctest)
// until Ghostty's HTM PR lands. Run with: ./et-test ghostty

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

// DaemonCreator leaves a double-forked `htm` process blocked in
// system("htmd") for the life of the daemon.  pgrep -x htm therefore
// stays true whenever htmd is running.  A real client has a TTY.
bool htmHasControllingTty() {
  string cmd = string("ps -U ") + to_string(selfUid()) + " -o tty=,comm=";
  FILE* fp = popen(cmd.c_str(), "r");
  if (!fp) {
    return false;
  }
  char line[256];
  bool found = false;
  while (fgets(line, sizeof(line), fp)) {
    char tty[64] = {0};
    char comm[64] = {0};
    if (sscanf(line, "%63s %63s", tty, comm) == 2 && strcmp(comm, "htm") == 0 &&
        strcmp(tty, "??") != 0) {
      found = true;
      break;
    }
  }
  pclose(fp);
  return found;
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

const string kInit = "\x1b[###q";
const string kExit = "\x1b[$$$q";

size_t longestInitPrefix(const string& data) {
  const size_t maxHold = std::min(data.size(), kInit.size() - 1);
  for (size_t n = maxHold; n > 0; n--) {
    if (kInit.compare(0, n, data, data.size() - n, n) == 0) {
      return n;
    }
  }
  return 0;
}

// Ghostty-like PTY owner: spawn real `htm`/`htmd`, intercept the packet
// stream the way Termio does, and inject framed packets the way a leader
// surface (INSERT_DEBUG_KEYS) and follower surfaces (INSERT_KEYS) do.
class GhosttyHtmPty {
 public:
  explicit GhosttyHtmPty(size_t readChunk, bool killOtherSessions = true)
      : readChunk(readChunk), killOtherSessions(killOtherSessions) {
    spawn();
  }

  ~GhosttyHtmPty() { closeSession(true); }

  void closeSession(bool killChild) {
    if (master >= 0) {
      ::close(master);
      master = -1;
    }
    if (pid > 0) {
      if (killChild) {
        ::kill(pid, SIGTERM);
      }
      int status = 0;
      waitUntil([&]() { return waitpid(pid, &status, WNOHANG) == pid; }, 4000);
      if (waitpid(pid, &status, WNOHANG) != pid) {
        ::kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
      }
      pid = -1;
    }
  }

  bool childAlive() {
    if (pid <= 0) {
      return false;
    }
    int status = 0;
    pid_t rc = waitpid(pid, &status, WNOHANG);
    if (rc == pid) {
      pid = -1;
      return false;
    }
    return true;
  }

  void writeRaw(const string& data) {
    size_t off = 0;
    const auto start = std::chrono::steady_clock::now();
    while (off < data.size()) {
      ssize_t n = ::write(master, data.data() + off, data.size() - off);
      if (n > 0) {
        off += size_t(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
        int saved = errno;
        pump(5);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        if (elapsed > 30000) {
          FAIL("Timed out writing to HTM PTY");
        }
        if (saved == EAGAIN) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        continue;
      }
      FAIL("write to HTM PTY failed: errno=" << errno);
    }
  }

  void writePacket(char header, const string& payload) {
    string framed;
    framed.push_back(header);
    framed += b64Int32(int32_t(payload.size()));
    framed += payload;
    writeRaw(framed);
  }

  void writeDebugKeys(const string& keys) {
    writePacket(INSERT_DEBUG_KEYS, keys);
  }

  void writeInsertKeys(const string& paneId, const string& keys) {
    writePacket(INSERT_KEYS, paneId + b64Bytes(keys));
  }

  void writeNewTab(const string& tabId, const string& paneId) {
    writePacket(NEW_TAB, tabId + paneId);
  }

  void writeNewSplit(const string& sourceId, const string& newId,
                     bool vertical) {
    string payload = sourceId + newId;
    payload.push_back(vertical ? '1' : '0');
    writePacket(NEW_SPLIT, payload);
  }

  void writeResize(const string& paneId, int32_t cols, int32_t rows) {
    writePacket(RESIZE_PANE, b64Int32(cols) + b64Int32(rows) + paneId);
  }

  void writeClosePane(const string& paneId) {
    writePacket(CLIENT_CLOSE_PANE, paneId);
  }

  // Pump PTY output the way Ghostty does:  optionally one byte at a time
  // so init/packet framing is split across reads.
  bool pump(int timeoutMs = 200) {
    const auto start = std::chrono::steady_clock::now();
    bool got = false;
    while (true) {
      char tmp[4096];
      size_t want = std::min(readChunk, sizeof(tmp));
      ssize_t n = ::read(master, tmp, want);
      if (n > 0) {
        got = true;
        consume(string(tmp, size_t(n)));
        continue;
      }
      if (n == 0) {
        eof = true;
        return got;
      }
      if (n < 0 && errno == EINTR) {
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

  bool waitForInit(int timeoutMs = 10000) {
    return waitFor([&]() { return haveInit; }, timeoutMs);
  }

  bool waitForHeader(char header, int timeoutMs = 8000) {
    return waitFor(
        [&]() {
          for (const auto& p : packets) {
            if (p.header == header) {
              return true;
            }
          }
          return false;
        },
        timeoutMs);
  }

  int headerCount(char header) const {
    int n = 0;
    for (const auto& p : packets) {
      if (p.header == header) {
        n++;
      }
    }
    return n;
  }

  string paneOutput(const string& paneId) const {
    string out;
    for (const auto& p : packets) {
      string id;
      string body;
      if (decodeAppendToPane(p, &id, &body) && id == paneId) {
        out.append(body);
      }
    }
    return out;
  }

  vector<string> appendPaneOrder(size_t fromIndex = 0) const {
    vector<string> order;
    for (size_t i = fromIndex; i < packets.size(); i++) {
      string id;
      string body;
      if (decodeAppendToPane(packets[i], &id, &body)) {
        order.push_back(id);
      }
    }
    return order;
  }

  string firstPaneId() const {
    REQUIRE(haveInit);
    REQUIRE(initState.contains("panes"));
    return firstJsonKey(initState["panes"]);
  }

  bool htmMode = false;
  bool sawExitSeq = false;
  bool eof = false;
  bool haveInit = false;
  json initState;
  vector<HtmPacket> packets;
  string debugLog;
  string preInit;
  pid_t pid = -1;

 private:
  void spawn() {
    REQUIRE(executable(htmPath()));
    REQUIRE(executable(htmdPath()));

    int slave = -1;
    REQUIRE(openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);

    struct winsize ws;
    ws.ws_row = 24;
    ws.ws_col = 80;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    ioctl(master, TIOCSWINSZ, &ws);

    fflush(NULL);
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
      if (killOtherSessions) {
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

  void consume(const string& chunk) {
    if (sawExitSeq) {
      return;
    }
    if (!htmMode) {
      string combined = pendingInit + chunk;
      auto pos = combined.find(kInit);
      if (pos == string::npos) {
        size_t hold = longestInitPrefix(combined);
        preInit.append(combined.substr(0, combined.size() - hold));
        pendingInit = combined.substr(combined.size() - hold);
        return;
      }
      preInit.append(combined.substr(0, pos));
      pendingInit.clear();
      htmMode = true;
      packetBuf = combined.substr(pos + kInit.size());
      drainPackets();
      return;
    }
    packetBuf.append(chunk);
    if (packetBuf.find(kExit) != string::npos) {
      sawExitSeq = true;
      htmMode = false;
      return;
    }
    drainPackets();
  }

  void drainPackets() {
    while (!holdPackets) {
      HtmPacket packet;
      string work = packetBuf;
      if (!popPacket(&work, &packet)) {
        return;
      }
      packetBuf = work;
      dispatch(packet);
    }
  }

  void dispatch(const HtmPacket& packet) {
    packets.push_back(packet);
    if (packet.header == SESSION_END) {
      htmMode = false;
      return;
    }
    if (packet.header == INIT_STATE) {
      holdPackets = true;
      initState = json::parse(packet.payload);
      haveInit = true;
      holdPackets = false;
      drainPackets();
      return;
    }
    if (packet.header == DEBUG_LOG) {
      string decoded;
      if (Base64::Decode(packet.payload, &decoded)) {
        debugLog.append(decoded);
      }
    }
  }

  int master = -1;
  size_t readChunk = 4096;
  bool killOtherSessions = true;
  string pendingInit;
  string packetBuf;
  bool holdPackets = false;
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

string findGhostty() {
  const char* env = getenv("GHOSTTY");
  if (env && executable(env)) {
    return env;
  }
  const char* candidates[] = {
      "/Users/jjg/github/ghostty/macos/build/Debug/Ghostty.app/Contents/MacOS/"
      "ghostty",
      "/Users/jjg/github/ghostty/zig-out/Ghostty.app/Contents/MacOS/ghostty",
      "/Users/jjg/github/ghostty/zig-out/bin/ghostty",
      "/Users/jjg/github/ghostty/macos/build/Build/Products/Debug/Ghostty.app/"
      "Contents/MacOS/ghostty",
      "/Applications/Ghostty.app/Contents/MacOS/ghostty",
  };
  for (const char* path : candidates) {
    if (executable(path)) {
      return path;
    }
  }
  return "";
}

string ghosttyAppBundle(const string& bin) {
  const string marker = ".app/Contents/MacOS/";
  auto pos = bin.rfind(marker);
  if (pos == string::npos) {
    return "";
  }
  return bin.substr(0, pos + 4);  // includes ".app"
}

void quitGhosttyApp(const string& app) {
  if (app.empty()) {
    return;
  }
  string osa =
      string("osascript -e 'tell application \"") + app + "\" to quit'";
  system(osa.c_str());
  waitUntil(
      [&]() {
        string cmd = string("pgrep -f ") + app + " >/dev/null 2>&1";
        return system(cmd.c_str()) != 0;
      },
      5000);
  string pkill = string("pkill -f ") + app + " >/dev/null 2>&1";
  system(pkill.c_str());
}

bool ghosttyHasHtmIntegration(const string& bin) {
  string cmd = string("'") + bin + "' +show-config --default 2>/dev/null";
  FILE* fp = popen(cmd.c_str(), "r");
  if (!fp) {
    return false;
  }
  string out;
  char buf[512];
  while (fgets(buf, sizeof(buf), fp)) {
    out.append(buf);
  }
  pclose(fp);
  return out.find("htm-bin-dir") != string::npos &&
         out.find("htm-integration") != string::npos;
}

}  // namespace

TEST_CASE("Ghostty-style PTY: init, keys, tab/split, escape, reconnect, x",
          "[Htm][.ghostty][e2e]") {
  if (!executable(htmPath()) || !executable(htmdPath())) {
    SKIP("htm/htmd are not built (expected in ET_BUILD_DIR or ./)");
  }
  IsolatedHtmd isolate;

  {
    GhosttyHtmPty ghostty(4096);
    REQUIRE(ghostty.waitForInit());
    REQUIRE(ghostty.htmMode);
    REQUIRE(ghostty.debugLog.find("HTM initialized") != string::npos);
    REQUIRE(htmdRunning());
    string pane = ghostty.firstPaneId();

    ghostty.writeInsertKeys(pane, "printf 'GHOSTTY_E2E_PANE\\n'\n");
    REQUIRE(ghostty.waitForHeader(APPEND_TO_PANE));

    string tabId = sole::uuid4().str();
    string tabPane = sole::uuid4().str();
    ghostty.writeNewTab(tabId, tabPane);

    string splitPane = sole::uuid4().str();
    ghostty.writeNewSplit(pane, splitPane, true);
    ghostty.writeResize(pane, 100, 30);

    ghostty.writeInsertKeys(tabPane, "printf 'GHOSTTY_E2E_TAB\\n'\n");
    REQUIRE(ghostty.waitFor(
        [&]() { return ghostty.headerCount(APPEND_TO_PANE) >= 2; }));

    // SERVER_CLOSE_PANE is emitted when a pane's shell exits, not when the
    // client sends CLIENT_CLOSE_PANE.
    ghostty.writeInsertKeys(splitPane, "printf 'GHOSTTY_E2E_SPLIT\\n'; exit\n");
    REQUIRE(ghostty.waitForHeader(SERVER_CLOSE_PANE, 10000));

    ghostty.writeClosePane(tabPane);

    // Escape on the leader: Ghostty wraps it as INSERT_DEBUG_KEYS.
    ghostty.writeDebugKeys(string(1, char(27)));
    REQUIRE(ghostty.waitFor(
        [&]() { return ghostty.sawExitSeq || !ghostty.childAlive(); }, 8000));
    ghostty.closeSession(false);
    REQUIRE_FALSE(ghostty.childAlive());
    REQUIRE(htmdRunning());
    REQUIRE_FALSE(htmHasControllingTty());
  }

  {
    GhosttyHtmPty ghostty(4096, false);
    REQUIRE(ghostty.waitForInit());
    REQUIRE(ghostty.initState["panes"].size() >= 1);
    ghostty.writeDebugKeys("x");
    REQUIRE(ghostty.waitFor(
        [&]() {
          return ghostty.sawExitSeq || !ghostty.childAlive() || !htmdRunning();
        },
        8000));
    ghostty.closeSession(false);
  }
  REQUIRE(waitUntil([]() { return !htmdRunning(); }, 8000));
}

TEST_CASE("Ghostty-style PTY: init sequence split across 1-byte reads",
          "[Htm][.ghostty][e2e][race]") {
  if (!executable(htmPath()) || !executable(htmdPath())) {
    SKIP("htm/htmd are not built");
  }
  IsolatedHtmd isolate;
  GhosttyHtmPty ghostty(1);
  REQUIRE(ghostty.waitForInit(15000));
  REQUIRE(ghostty.htmMode);
  REQUIRE(ghostty.haveInit);
  ghostty.writeDebugKeys("x");
  ghostty.waitFor([&]() { return !ghostty.childAlive() || !htmdRunning(); },
                  8000);
}

TEST_CASE("Ghostty-style PTY: rapid escape/reconnect does not wedge htmd",
          "[Htm][.ghostty][e2e][race]") {
  if (!executable(htmPath()) || !executable(htmdPath())) {
    SKIP("htm/htmd are not built");
  }
  IsolatedHtmd isolate;
  for (int i = 0; i < 5; i++) {
    GhosttyHtmPty ghostty(3, i == 0);
    REQUIRE(ghostty.waitForInit(15000));
    ghostty.writeDebugKeys(string(1, char(27)));
    REQUIRE(ghostty.waitFor(
        [&]() { return ghostty.sawExitSeq || !ghostty.childAlive(); }, 8000));
    ghostty.closeSession(false);
    REQUIRE_FALSE(ghostty.childAlive());
    REQUIRE_FALSE(htmHasControllingTty());
    REQUIRE(htmdRunning());
  }
  GhosttyHtmPty last(4096, false);
  REQUIRE(last.waitForInit());
  last.writeDebugKeys("x");
  last.waitFor([&]() { return !htmdRunning(); }, 8000);
}

TEST_CASE("Ghostty-style PTY: SIGTERM writes exit sequence and htmd survives",
          "[Htm][.ghostty][e2e]") {
  if (!executable(htmPath()) || !executable(htmdPath())) {
    SKIP("htm/htmd are not built");
  }
  IsolatedHtmd isolate;
  GhosttyHtmPty ghostty(4096);
  REQUIRE(ghostty.waitForInit());
  REQUIRE(::kill(ghostty.pid, SIGTERM) == 0);
  REQUIRE(ghostty.waitFor(
      [&]() { return ghostty.sawExitSeq || !ghostty.childAlive(); }, 8000));
  ghostty.closeSession(false);
  REQUIRE_FALSE(ghostty.childAlive());
  REQUIRE_FALSE(htmHasControllingTty());
  REQUIRE(htmdRunning());
  GhosttyHtmPty reconnect(4096, false);
  REQUIRE(reconnect.waitForInit());
  reconnect.writeDebugKeys("x");
  reconnect.waitFor([&]() { return !htmdRunning(); }, 8000);
}

TEST_CASE("Ghostty-style PTY: SIGKILL of htm lets htmd accept a new client",
          "[Htm][.ghostty][e2e][race]") {
  if (!executable(htmPath()) || !executable(htmdPath())) {
    SKIP("htm/htmd are not built");
  }
  IsolatedHtmd isolate;
  pid_t killed = -1;
  {
    GhosttyHtmPty ghostty(4096);
    REQUIRE(ghostty.waitForInit());
    killed = ghostty.pid;
    REQUIRE(::kill(killed, SIGKILL) == 0);
    ghostty.closeSession(false);
  }
  REQUIRE_FALSE(htmHasControllingTty());
  REQUIRE(htmdRunning());
  GhosttyHtmPty reconnect(4096, false);
  REQUIRE(reconnect.waitForInit());
  reconnect.writeDebugKeys("x");
  reconnect.waitFor([&]() { return !htmdRunning(); }, 8000);
}

TEST_CASE("Ghostty-style PTY: closing the last pane shuts down htmd",
          "[Htm][.ghostty][e2e]") {
  if (!executable(htmPath()) || !executable(htmdPath())) {
    SKIP("htm/htmd are not built");
  }
  IsolatedHtmd isolate;
  GhosttyHtmPty ghostty(4096);
  REQUIRE(ghostty.waitForInit());
  string pane = ghostty.firstPaneId();
  ghostty.writeClosePane(pane);
  REQUIRE(waitUntil(
      [&]() {
        ghostty.pump(50);
        return !htmdRunning() || !ghostty.childAlive() || ghostty.sawExitSeq;
      },
      10000));
  ghostty.closeSession(true);
  REQUIRE(waitUntil([]() { return !htmdRunning(); }, 8000));
}

TEST_CASE("Ghostty-style PTY: wrapped newline does not disconnect htmd",
          "[Htm][.ghostty][e2e]") {
  if (!executable(htmPath()) || !executable(htmdPath())) {
    SKIP("htm/htmd are not built");
  }
  IsolatedHtmd isolate;
  GhosttyHtmPty ghostty(4096);
  REQUIRE(ghostty.waitForInit());
  // A raw newline on the IPC is a protocol error. Ghostty must wrap leader
  // keystrokes as INSERT_DEBUG_KEYS so leftover newlines are safe.
  ghostty.writeDebugKeys("\n");
  ghostty.pump(400);
  REQUIRE(ghostty.htmMode);
  REQUIRE(htmdRunning());
  REQUIRE(ghostty.childAlive());
  ghostty.writeDebugKeys("x");
  ghostty.waitFor([&]() { return !htmdRunning(); }, 8000);
}

string burstCmd(const string& tag, int n) {
  return "i=1; while [ \"$i\" -le " + to_string(n) + " ]; do printf '" + tag +
         "_%s\\n' \"$i\"; i=$((i+1)); sleep 0.04; done\n";
}

TEST_CASE("Ghostty-style PTY: tabs, nested splits, concurrent pane output",
          "[Htm][.ghostty][e2e][features]") {
  if (!executable(htmPath()) || !executable(htmdPath())) {
    SKIP("htm/htmd are not built");
  }
  IsolatedHtmd isolate;
  const int bursts = 8;

  string p0;
  string pane1;
  string pane2;
  string splitV;
  string splitH;
  {
    GhosttyHtmPty ghostty(4096);
    REQUIRE(ghostty.waitForInit());
    p0 = ghostty.firstPaneId();

    string tab1 = sole::uuid4().str();
    pane1 = sole::uuid4().str();
    ghostty.writeNewTab(tab1, pane1);

    string tab2 = sole::uuid4().str();
    pane2 = sole::uuid4().str();
    ghostty.writeNewTab(tab2, pane2);

    splitV = sole::uuid4().str();
    ghostty.writeNewSplit(p0, splitV, true);
    splitH = sole::uuid4().str();
    ghostty.writeNewSplit(p0, splitH, false);

    ghostty.writeResize(p0, 60, 20);
    ghostty.writeResize(splitV, 60, 20);
    ghostty.writeResize(pane1, 80, 24);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ghostty.pump(200);

    const vector<pair<string, string>> panes = {
        {p0, "HTM_C0"},     {pane1, "HTM_C1"},  {pane2, "HTM_C2"},
        {splitV, "HTM_C3"}, {splitH, "HTM_C4"},
    };
    size_t burstAt = ghostty.packets.size();
    for (const auto& pane : panes) {
      ghostty.writeInsertKeys(pane.first, burstCmd(pane.second, bursts));
    }

    REQUIRE(ghostty.waitFor(
        [&]() {
          for (const auto& pane : panes) {
            if (ghostty.paneOutput(pane.first)
                    .find(pane.second + "_" + to_string(bursts)) ==
                string::npos) {
              return false;
            }
          }
          return true;
        },
        15000));

    for (const auto& pane : panes) {
      string out = ghostty.paneOutput(pane.first);
      REQUIRE(out.find(pane.second + "_1") != string::npos);
      REQUIRE(out.find(pane.second + "_" + to_string(bursts)) != string::npos);
    }

    vector<string> order = ghostty.appendPaneOrder(burstAt);
    REQUIRE(order.size() >= 10);
    std::set<string> unique(order.begin(), order.end());
    REQUIRE(unique.size() >= 4);
    int transitions = 0;
    for (size_t i = 1; i < order.size(); i++) {
      if (order[i] != order[i - 1]) {
        transitions++;
      }
    }
    REQUIRE(transitions >= 4);

    ghostty.writeClosePane(splitH);
    ghostty.writeInsertKeys(pane1, "printf 'HTM_AFTER_CLOSE\\n'\n");
    REQUIRE(ghostty.waitFor(
        [&]() {
          return ghostty.paneOutput(pane1).find("HTM_AFTER_CLOSE") !=
                 string::npos;
        },
        8000));

    ghostty.writeDebugKeys(string(1, char(27)));
    REQUIRE(ghostty.waitFor(
        [&]() { return ghostty.sawExitSeq || !ghostty.childAlive(); }, 8000));
    ghostty.closeSession(false);
  }

  {
    GhosttyHtmPty ghostty(4096, false);
    REQUIRE(ghostty.waitForInit());
    REQUIRE(ghostty.initState["tabs"].size() == 3);
    REQUIRE(ghostty.initState["panes"].size() == 4);
    REQUIRE(ghostty.initState["splits"].size() >= 1);
    REQUIRE(ghostty.initState["panes"].contains(p0));
    REQUIRE(ghostty.initState["panes"].contains(pane1));
    REQUIRE(ghostty.initState["panes"].contains(pane2));
    REQUIRE(ghostty.initState["panes"].contains(splitV));
    REQUIRE(ghostty.waitFor(
        [&]() {
          return ghostty.paneOutput(p0).find("HTM_C0_") != string::npos &&
                 ghostty.paneOutput(pane1).find("HTM_C1_") != string::npos &&
                 ghostty.paneOutput(pane2).find("HTM_C2_") != string::npos &&
                 ghostty.paneOutput(splitV).find("HTM_C3_") != string::npos;
        },
        8000));
    ghostty.writeDebugKeys("x");
    ghostty.waitFor([&]() { return !htmdRunning(); }, 8000);
  }
}

// Several panes printing while the leader keeps injecting keys, with a slow
// PTY drain so stdout can back up the way Hyper did. writeRaw times out if
// htm stops reading stdin (the old blocking writeAll(stdout) deadlock).
TEST_CASE("Ghostty-style PTY: keys keep flowing under stdout backpressure",
          "[Htm][.ghostty][e2e][stress]") {
  if (!executable(htmPath()) || !executable(htmdPath())) {
    SKIP("htm/htmd are not built");
  }
  IsolatedHtmd isolate;
  GhosttyHtmPty ghostty(256);
  REQUIRE(ghostty.waitForInit());
  string p0 = ghostty.firstPaneId();
  string tabPane = sole::uuid4().str();
  ghostty.writeNewTab(sole::uuid4().str(), tabPane);
  string splitPane = sole::uuid4().str();
  ghostty.writeNewSplit(p0, splitPane, true);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  ghostty.pump(150);

  auto bgBurst = [](const string& tag, int n) {
    string cmd = burstCmd(tag, n);
    if (!cmd.empty() && cmd.back() == '\n') {
      cmd.pop_back();
    }
    return cmd + " &\n";
  };
  const int bursts = 20;
  ghostty.writeInsertKeys(p0, bgBurst("BP0", bursts));
  ghostty.writeInsertKeys(tabPane, bgBurst("BP1", bursts));
  ghostty.writeInsertKeys(splitPane, bgBurst("BP2", bursts));
  ghostty.pump(200);

  const int keyMarks = 12;
  for (int i = 0; i < keyMarks; i++) {
    ghostty.writeInsertKeys(p0, "printf 'BPKEY_" + to_string(i) + "\\n'\n");
    ghostty.pump(20);
  }

  REQUIRE(ghostty.waitFor(
      [&]() {
        string out = ghostty.paneOutput(p0);
        return out.find("BPKEY_" + to_string(keyMarks - 1)) != string::npos &&
               out.find("BP0_1") != string::npos && ghostty.childAlive();
      },
      15000));
  INFO("p0 output: " << ghostty.paneOutput(p0));
  REQUIRE(ghostty.paneOutput(tabPane).find("BP1_1") != string::npos);
  REQUIRE(ghostty.paneOutput(splitPane).find("BP2_1") != string::npos);
  REQUIRE(ghostty.childAlive());

  ghostty.writeDebugKeys("x");
  ghostty.waitFor([&]() { return !htmdRunning(); }, 8000);
}

TEST_CASE("Ghostty-style PTY: concurrent read/write on many tabs and splits",
          "[Htm][.ghostty][e2e][stress]") {
  if (!executable(htmPath()) || !executable(htmdPath())) {
    SKIP("htm/htmd are not built");
  }
  IsolatedHtmd isolate;
  GhosttyHtmPty ghostty(4096);
  REQUIRE(ghostty.waitForInit());

  string p0 = ghostty.firstPaneId();
  vector<pair<string, string>> panes = {{p0, "P0"}};
  for (int t = 0; t < 3; t++) {
    string tabPane = sole::uuid4().str();
    ghostty.writeNewTab(sole::uuid4().str(), tabPane);
    char tag[8];
    snprintf(tag, sizeof(tag), "T%d", t);
    panes.push_back({tabPane, tag});
    string splitPane = sole::uuid4().str();
    ghostty.writeNewSplit(tabPane, splitPane, t % 2 == 0);
    snprintf(tag, sizeof(tag), "S%d", t);
    panes.push_back({splitPane, tag});
  }
  string split0 = sole::uuid4().str();
  ghostty.writeNewSplit(p0, split0, true);
  panes.push_back({split0, "PX"});
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  ghostty.pump(200);

  // Login prompts sit on a partial line. Keep unique tags on their own short
  // lines so an 80-column wrap cannot split OUT_P0_0123 from the sequence
  // number; bulky padding goes on the following line.
  for (const auto& pane : panes) {
    ghostty.writeResize(pane.first, 200, 40);
    ghostty.writeInsertKeys(pane.first, "\n");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ghostty.pump(150);

  const int floodLines = 400;
  const int keyRounds = 24;
  const string pad(64, 'X');
  size_t burstAt = ghostty.packets.size();
  for (const auto& pane : panes) {
    string cmd = "i=1; while [ \"$i\" -le " + to_string(floodLines) +
                 " ]; do printf 'OUT_" + pane.second +
                 "_%04d\\n' \"$i\"; printf '" + pad +
                 "\\n'; i=$((i+1)); done &\n";
    ghostty.writeInsertKeys(pane.first, cmd);
  }
  ghostty.pump(50);

  for (int round = 0; round < keyRounds; round++) {
    for (const auto& pane : panes) {
      char buf[80];
      snprintf(buf, sizeof(buf), "printf 'IN_%s_%04d\\n'\n",
               pane.second.c_str(), round);
      ghostty.writeInsertKeys(pane.first, buf);
    }
    if (round % 8 == 0) {
      for (const auto& pane : panes) {
        ghostty.writeResize(pane.first, 200, 40);
      }
    }
    ghostty.pump(10);
    REQUIRE(ghostty.childAlive());
  }

  REQUIRE(ghostty.waitFor(
      [&]() {
        if (!ghostty.childAlive()) {
          return false;
        }
        for (const auto& pane : panes) {
          string out = ghostty.paneOutput(pane.first);
          char lastOut[32];
          char lastIn[32];
          snprintf(lastOut, sizeof(lastOut), "OUT_%s_%04d", pane.second.c_str(),
                   floodLines);
          snprintf(lastIn, sizeof(lastIn), "IN_%s_%04d", pane.second.c_str(),
                   keyRounds - 1);
          if (out.find(lastOut) == string::npos ||
              out.find(lastIn) == string::npos) {
            return false;
          }
        }
        return true;
      },
      90000));
  REQUIRE(ghostty.childAlive());
  REQUIRE(htmdRunning());

  for (const auto& pane : panes) {
    string out = ghostty.paneOutput(pane.first);
    int outHits = 0;
    int inHits = 0;
    string missing;
    for (int i = 1; i <= floodLines; i++) {
      char needle[32];
      snprintf(needle, sizeof(needle), "OUT_%s_%04d", pane.second.c_str(), i);
      if (out.find(needle) != string::npos) {
        outHits++;
      } else if (missing.size() < 80) {
        if (!missing.empty()) {
          missing += ",";
        }
        missing += to_string(i);
      }
    }
    for (int i = 0; i < keyRounds; i++) {
      char needle[32];
      snprintf(needle, sizeof(needle), "IN_%s_%04d", pane.second.c_str(), i);
      if (out.find(needle) != string::npos) {
        inHits++;
      }
    }
    INFO("pane " << pane.second << " outHits=" << outHits
                 << " inHits=" << inHits << " missing=" << missing);
    // Background flood and foreground key printfs share one PTY, so a few
    // markers can be split at the byte level. Require almost all of them,
    // and that no other pane's tags leaked into this stream.
    REQUIRE(outHits >= floodLines - keyRounds);
    REQUIRE(inHits >= keyRounds - 2);
    for (const auto& other : panes) {
      if (other.second == pane.second) {
        continue;
      }
      REQUIRE(out.find("OUT_" + other.second + "_") == string::npos);
      REQUIRE(out.find("IN_" + other.second + "_") == string::npos);
    }
  }

  vector<string> order = ghostty.appendPaneOrder(burstAt);
  std::set<string> unique(order.begin(), order.end());
  REQUIRE(unique.size() >= 6);
  int transitions = 0;
  for (size_t i = 1; i < order.size(); i++) {
    if (order[i] != order[i - 1]) {
      transitions++;
    }
  }
  REQUIRE(transitions >= 20);

  ghostty.writeDebugKeys("x");
  ghostty.waitFor([&]() { return !htmdRunning(); }, 8000);
}

TEST_CASE("Ghostty GUI: attach to a multi-pane HTM session",
          "[Htm][.ghostty][e2e][gui][features]") {
  string ghosttyBin = findGhostty();
  if (ghosttyBin.empty()) {
    SKIP("Ghostty is not installed; set GHOSTTY to an HTM-enabled binary");
  }
  if (!ghosttyHasHtmIntegration(ghosttyBin)) {
    SKIP("Ghostty at " << ghosttyBin << " has no htm-integration");
  }
  if (!executable(htmPath()) || !executable(htmdPath())) {
    SKIP("htm/htmd are not built");
  }
  IsolatedHtmd isolate;

  {
    GhosttyHtmPty session(4096);
    REQUIRE(session.waitForInit());
    string p0 = session.firstPaneId();
    string tab1 = sole::uuid4().str();
    string pane1 = sole::uuid4().str();
    session.writeNewTab(tab1, pane1);
    string split = sole::uuid4().str();
    session.writeNewSplit(p0, split, true);
    session.writeInsertKeys(p0, "printf 'GUI_TAB0\\n'\n");
    session.writeInsertKeys(pane1, "printf 'GUI_TAB1\\n'\n");
    session.writeInsertKeys(split, "printf 'GUI_SPLIT\\n'\n");
    REQUIRE(session.waitFor(
        [&]() {
          return session.paneOutput(p0).find("GUI_TAB0") != string::npos &&
                 session.paneOutput(pane1).find("GUI_TAB1") != string::npos &&
                 session.paneOutput(split).find("GUI_SPLIT") != string::npos;
        },
        10000));
    session.writeDebugKeys(string(1, char(27)));
    REQUIRE(session.waitFor(
        [&]() { return session.sawExitSeq || !session.childAlive(); }, 8000));
    session.closeSession(false);
  }
  REQUIRE(htmdRunning());

  string dir;
  char resolved[PATH_MAX];
  if (realpath(htmBinDir().c_str(), resolved)) {
    dir = resolved;
  } else {
    dir = htmBinDir();
  }
  string htm = dir + "/htm";
  string binDirFlag = string("--htm-bin-dir=") + dir;
  string app = ghosttyAppBundle(ghosttyBin);
  if (app.empty()) {
    SKIP("Ghostty binary is not inside a .app bundle");
  }

  pid_t opener = fork();
  REQUIRE(opener >= 0);
  if (opener == 0) {
    execl("/usr/bin/open", "open", "-na", app.c_str(), "--args",
          "--quit-after-last-window-closed=true",
          "--confirm-close-surface=false", "--window-save-state=never",
          binDirFlag.c_str(), "-e", htm.c_str(), (char*)nullptr);
    _exit(127);
  }
  int openStatus = 0;
  waitpid(opener, &openStatus, 0);
  REQUIRE(WIFEXITED(openStatus));
  REQUIRE(WEXITSTATUS(openStatus) == 0);

  bool attached = waitUntil(
      []() { return htmdRunning() && htmHasControllingTty(); }, 25000);
  if (!attached) {
    quitGhosttyApp(app);
    FAIL("Ghostty did not attach to the existing HTM session");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  quitGhosttyApp(app);
  REQUIRE(waitUntil([]() { return !htmHasControllingTty(); }, 8000));
  REQUIRE(htmdRunning());

  {
    GhosttyHtmPty reconnect(4096, false);
    REQUIRE(reconnect.waitForInit());
    REQUIRE(reconnect.initState["tabs"].size() >= 2);
    REQUIRE(reconnect.initState["panes"].size() >= 3);
    REQUIRE(reconnect.initState["splits"].size() >= 1);
    reconnect.writeDebugKeys("x");
    reconnect.waitFor([&]() { return !htmdRunning(); }, 8000);
  }
}

TEST_CASE("Ghostty GUI: launch htm, observe htmd, quit cleanly",
          "[Htm][.ghostty][e2e][gui]") {
  string ghostty = findGhostty();
  if (ghostty.empty()) {
    SKIP("Ghostty is not installed; set GHOSTTY to an HTM-enabled binary");
  }
  if (!ghosttyHasHtmIntegration(ghostty)) {
    SKIP("Ghostty at "
         << ghostty
         << " has no htm-integration (need the htm-integration branch)");
  }
  if (!executable(htmPath()) || !executable(htmdPath())) {
    SKIP("htm/htmd are not built");
  }
  IsolatedHtmd isolate;

  string dir;
  char resolved[PATH_MAX];
  if (realpath(htmBinDir().c_str(), resolved)) {
    dir = resolved;
  } else {
    dir = htmBinDir();
  }
  string htm = dir + "/htm";
  string binDirFlag = string("--htm-bin-dir=") + dir;

#if defined(__APPLE__)
  // macOS Ghostty is a Swift app: the CLI binary cannot open windows.
  string app = ghosttyAppBundle(ghostty);
  if (app.empty()) {
    SKIP("Ghostty binary is not inside a .app bundle");
  }
  pid_t opener = fork();
  REQUIRE(opener >= 0);
  if (opener == 0) {
    execl("/usr/bin/open", "open", "-na", app.c_str(), "--args",
          "--quit-after-last-window-closed=true",
          "--confirm-close-surface=false", "--window-save-state=never",
          binDirFlag.c_str(), "-e", htm.c_str(), "-x", (char*)nullptr);
    _exit(127);
  }
  int openStatus = 0;
  waitpid(opener, &openStatus, 0);
  REQUIRE(WIFEXITED(openStatus));
  REQUIRE(WEXITSTATUS(openStatus) == 0);
#else
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    string path = dir + ":" + (getenv("PATH") ? getenv("PATH") : "");
    setenv("PATH", path.c_str(), 1);
    execl(ghostty.c_str(), "ghostty", "--quit-after-last-window-closed=true",
          "--confirm-close-surface=false", "--window-save-state=never",
          binDirFlag.c_str(), "-e", htm.c_str(), "-x", (char*)nullptr);
    _exit(127);
  }
#endif

  bool inited = waitUntil(
      []() { return htmdRunning() && access(ipcPath().c_str(), F_OK) == 0; },
      25000);
  if (!inited) {
#if defined(__APPLE__)
    quitGhosttyApp(ghosttyAppBundle(ghostty));
#else
    ::kill(pid, SIGTERM);
    int st = 0;
    waitpid(pid, &st, 0);
#endif
    FAIL("Ghostty did not start htmd / IPC socket");
  }

  // Give INIT_STATE time to land, then tear the GUI down. Ghostty should
  // close the leader PTY; htm must not leave a wedged htmd behind.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
#if defined(__APPLE__)
  quitGhosttyApp(ghosttyAppBundle(ghostty));
#else
  ::kill(pid, SIGTERM);
  REQUIRE(waitUntil(
      [&]() {
        int st = 0;
        return waitpid(pid, &st, WNOHANG) == pid;
      },
      8000));
  if (waitpid(pid, nullptr, WNOHANG) != pid) {
    ::kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
  }
#endif

  REQUIRE(waitUntil([]() { return !htmHasControllingTty(); }, 8000));
  // htmd is a daemon: it should still be running after the client disconnects
  // so a later `htm` can reconnect. It must also still accept a new client.
  REQUIRE(htmdRunning());
  {
    GhosttyHtmPty reconnect(4096, false);
    REQUIRE(reconnect.waitForInit());
    reconnect.writeDebugKeys("x");
    reconnect.waitFor([&]() { return !htmdRunning(); }, 8000);
  }
}

#endif  // WIN32
