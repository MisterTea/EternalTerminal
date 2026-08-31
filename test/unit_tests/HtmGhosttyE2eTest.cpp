#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#include <vector>

#include "HtmHeaderCodes.hpp"
#include "HtmTestHelpers.hpp"
#include "TestHeaders.hpp"

#ifndef WIN32
#if defined(__APPLE__)
#include <util.h>
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

string ipcPath() { return string(_PATH_TMP) + "htm." + GetHtmIpcUser() + ".ipc"; }

bool htmdRunning() {
  string cmd = string("pgrep -x -U ") + to_string(selfUid()) + " htmd >/dev/null 2>&1";
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
  string cmd = string("pkill -x -U ") + to_string(selfUid()) + " htmd >/dev/null 2>&1";
  system(cmd.c_str());
  waitUntil([]() { return !htmdRunning(); }, 3000);
}

void killHtmClients() {
  string cmd = string("pkill -x -U ") + to_string(selfUid()) + " htm >/dev/null 2>&1";
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
      waitUntil(
          [&]() { return waitpid(pid, &status, WNOHANG) == pid; }, 4000);
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
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        if (elapsed > 3000) {
          FAIL("Timed out writing to HTM PTY");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
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

  void writeNewSplit(const string& sourceId, const string& newId, bool vertical) {
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
    lockDir = string(_PATH_TMP) + "htm.e2e." + to_string(selfUid()) + ".lockdir";
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
      "/Users/jjg/github/ghostty/macos/build/Debug/Ghostty.app/Contents/MacOS/ghostty",
      "/Users/jjg/github/ghostty/zig-out/Ghostty.app/Contents/MacOS/ghostty",
      "/Users/jjg/github/ghostty/zig-out/bin/ghostty",
      "/Users/jjg/github/ghostty/macos/build/Build/Products/Debug/Ghostty.app/Contents/MacOS/ghostty",
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
  string osa = string("osascript -e 'tell application \"") + app + "\" to quit'";
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
          "[Htm][Ghostty][e2e]") {
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
    ghostty.writeInsertKeys(splitPane, "exit\n");
    REQUIRE(ghostty.waitForHeader(SERVER_CLOSE_PANE, 8000));

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
          "[Htm][Ghostty][e2e][race]") {
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
          "[Htm][Ghostty][e2e][race]") {
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
          "[Htm][Ghostty][e2e]") {
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
          "[Htm][Ghostty][e2e][race]") {
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
          "[Htm][Ghostty][e2e]") {
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
          "[Htm][Ghostty][e2e]") {
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

TEST_CASE("Ghostty GUI: launch htm, observe htmd, quit cleanly",
          "[Htm][Ghostty][e2e][gui]") {
  string ghostty = findGhostty();
  if (ghostty.empty()) {
    SKIP("Ghostty is not installed; set GHOSTTY to an HTM-enabled binary");
  }
  if (!ghosttyHasHtmIntegration(ghostty)) {
    SKIP("Ghostty at " << ghostty
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
      []() {
        return htmdRunning() && access(ipcPath().c_str(), F_OK) == 0;
      },
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
