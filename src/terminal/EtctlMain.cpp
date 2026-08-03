/*
 * etctl: the client-side control CLI for backgrounded `et --ctl` sessions.
 *
 * It is a thin, stateless translator: each invocation resolves a session's local
 * control socket (~/.et/sessions/<name>.sock), sends one native control frame,
 * and prints the response.  The transport carries ET's own vocabulary (raw input
 * bytes and scrollback reads); the verbs here are ergonomic sugar composed from
 * it.
 */
#include <algorithm>
#include <csignal>
#include <cxxopts.hpp>
#include <map>
#include <regex>
#include <sstream>

#include "ControlPaths.hpp"
#include "ControlProtocol.hpp"
#include "ETerminal.pb.h"
#include "Headers.hpp"
#include "Osc133.hpp"
#include "SessionCredentials.hpp"

using namespace et;

namespace {

/*
 * Ctrl-C handling.  et-lib's easyloggingpp installs a crash handler that
 * catches SIGINT and aborts with a "CRASH HANDLED" backtrace; but here Ctrl-C
 * is the normal way to stop a blocking verb (run, expect, wait, read --follow,
 * peep
 * --follow).  main() runs after easyloggingpp's static init, so we replace its
 * handler with one that exits cleanly using the conventional code 130.  The
 * interactive attach/observe verbs put the terminal in raw mode (ISIG off), so
 * there Ctrl-C arrives as a byte -- forwarded to the remote, or (in observe) a
 * local quit -- and this signal never fires.
 */
void onInterrupt(int) { _exit(130); }
void installInterruptHandler() {
  struct sigaction sa = {};
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = onInterrupt;
  sigaction(SIGINT, &sa, nullptr);
}

// One-line description for a subcommand (shown above its Usage:).
string descFor(const string& cmd) {
  if (cmd == "open")
    return "Start a control session in the background (idempotent).";
  if (cmd == "sessions") return "List local control sessions.";
  if (cmd == "gc")
    return "Remove dead session sockets (and, with --idle, end idle sessions).";
  if (cmd == "info")
    return "Show session status (liveness, link, size, cursor).";
  if (cmd == "kill") return "Force-stop a session's local daemon.";
  if (cmd == "read") return "Read session output without consuming it.";
  if (cmd == "write") return "Inject raw input bytes (a TEXT arg, or stdin).";
  if (cmd == "writeln") return "Inject a line of input (or a hidden password).";
  if (cmd == "run")
    return "Run a command; print its output verbatim, exit with its code.";
  if (cmd == "expect") return "Wait for a pattern to appear in the output.";
  return "";
}

/*
 * Build the cxxopts parser for a subcommand (drives both parsing and --help).
 * Positionals go in a hidden group so help({""}) lists only real options; the
 * synopsis after the program name is set via custom_help.
 */
cxxopts::Options buildOptions(const string& cmd) {
  cxxopts::Options o("etctl " + cmd, descFor(cmd));
  o.add_options()("h,help", "Print help");
  auto pos = o.add_options("positional");
  string synopsis = "NAME";
  if (cmd == "sessions") {
    synopsis = "";
  } else if (cmd == "info") {
    pos("NAME", "session name or socket path", cxxopts::value<string>());
    o.parse_positional({"NAME"});
    synopsis = "NAME";
  } else if (cmd == "kill") {
    o.add_options()(
        "wait",
        "Block until the session has actually ended (default 10s, --wait=S)",
        cxxopts::value<double>()->implicit_value("10"));
    pos("NAME", "session name or socket path", cxxopts::value<string>());
    o.parse_positional({"NAME"});
    synopsis = "NAME [--wait[=S]]";
  } else if (cmd == "read") {
    o.add_options()("cursor",
                    "Start at byte offset N (default: oldest retained)",
                    cxxopts::value<long long>())(
        "timeout", "Wait up to S seconds for new output, then return",
        cxxopts::value<double>());
    pos("NAME", "session", cxxopts::value<string>());
    o.parse_positional({"NAME"});
    synopsis = "NAME [OPTION...]";
  } else if (cmd == "write") {
    pos("NAME", "session", cxxopts::value<string>())(
        "TEXT", "text to send (raw, no newline added)",
        cxxopts::value<string>());
    o.parse_positional({"NAME", "TEXT"});
    synopsis = "NAME [TEXT]";
  } else if (cmd == "writeln") {
    o.add_options()("secret",
                    "Read the line hidden via getpass (e.g. a password)");
    pos("NAME", "session", cxxopts::value<string>())("TEXT", "text to send",
                                                     cxxopts::value<string>());
    o.parse_positional({"NAME", "TEXT"});
    synopsis = "NAME [TEXT] [--secret]";
  } else if (cmd == "run") {
    o.add_options()("timeout", "Seconds before giving up",
                    cxxopts::value<double>()->default_value("60"))(
        "framing",
        "Run framing: auto (detect, the default), osc133 (bracketed paste + OSC "
        "133; cleanest, needs shell integration), or mark (echo-marker here-doc; "
        "works on any shell)",
        cxxopts::value<string>()->default_value("auto"));
    pos("NAME", "session", cxxopts::value<string>())("CMD", "command to run",
                                                     cxxopts::value<string>());
    o.parse_positional({"NAME", "CMD"});
    synopsis = "NAME CMD [OPTION...]";
  } else if (cmd == "expect") {
    o.add_options()("timeout", "Seconds before giving up",
                    cxxopts::value<double>()->default_value("30"))(
        "exact", "Match PATTERN as a literal substring, not a regex")(
        "cursor",
        "Scan output from byte offset N (capture it before writing "
        "to avoid races)",
        cxxopts::value<long long>());
    pos("NAME", "session", cxxopts::value<string>())(
        "PATTERN", "regex (or literal with --exact)", cxxopts::value<string>());
    o.parse_positional({"NAME", "PATTERN"});
    synopsis = "NAME PATTERN [OPTION...]";
  } else if (cmd == "gc") {
    o.add_options()(
        "idle",
        "Also end live sessions idle longer than DUR (e.g. 30m, 6h; "
        "default 8h)",
        cxxopts::value<string>()->implicit_value("8h"))(
        "force", "Stop idle sessions outright, skipping the graceful eof");
    synopsis = "[--idle [DUR]] [--force]";
  }
  o.positional_help("");
  o.custom_help(synopsis);
  return o;
}

// Top-level overview: ET-style header + Usage + a flat, ordered command list.
void printOverview() {
  fprintf(stderr,
          "Control backgrounded Eternal Terminal (et --ctl) sessions\n"
          "Usage:\n"
          "  etctl <command> [args]\n"
          "\n"
          "  Open a reconnectable session in the background, then read its "
          "output and\n"
          "  send it input. Run 'etctl <command> --help' for a command's "
          "options.\n"
          "  NAME is a session name (under ~/.et/sessions, or "
          "$ET_SESSION_DIR) or a socket path.\n"
          "\n"
          "  open        start a control session in the background (idempotent)\n"
          "  run         run a command; capture its verbatim output + exit code\n"
          "  read        read output (non-destructive)\n"
          "  write       inject raw input bytes (no newline)\n"
          "  writeln     inject a line (or a hidden password)\n"
          "  expect      wait for a pattern in the output\n"
          "  info        show session status\n"
          "  sessions    list local control sessions\n"
          "  kill        force-stop a session daemon\n"
          "  gc          remove dead session sockets\n"
          "\n"
          "  -h, --help     show this overview (or `etctl <command> --help`)\n"
          "  -v, --version  print the etctl version\n");
}

string resolveSocketPath(const string& nameOrPath) {
  struct stat st;
  if (::stat(nameOrPath.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
    return nameOrPath;
  }
  return control_paths::socketPathForName(nameOrPath);
}

int connectControl(const string& name) {
  string path = resolveSocketPath(name);
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return -1;
  }
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

/*
 * One request, one response.  Returns false if the session is unreachable or
 * replied with an error; prints a diagnostic unless `quiet` (streaming readers
 * pass quiet and report their own clean "session ended" instead).
 */
bool oneShot(const string& name, uint8_t opcode, const string& payload,
             uint8_t* respOpcode, string* respPayload, bool quiet = false) {
  int fd = connectControl(name);
  if (fd < 0) {
    if (!quiet) {
      // A session that ended leaves a note saying why. Prefer it to errno,
      // which can only say the socket is missing and cannot distinguish a
      // session that ended from a name that never existed.
      const int savedErrno = errno;
      const string ended = session_creds::readTombstone(name);
      if (!ended.empty()) {
        fprintf(stderr, "etctl: session '%s' ended: %s\n", name.c_str(),
                ended.c_str());
      } else {
        fprintf(stderr, "etctl: cannot reach session '%s' (%s)\n", name.c_str(),
                strerror(savedErrno));
      }
    }
    return false;
  }
  bool ok = false;
  try {
    control_proto::writeFrame(fd, opcode, payload);
    ok = control_proto::readFrame(fd, respOpcode, respPayload);
  } catch (const std::exception& e) {
    if (!quiet)
      fprintf(stderr, "etctl: io error talking to '%s': %s\n", name.c_str(),
              e.what());
  }
  ::close(fd);
  if (!ok) {
    if (!quiet) fprintf(stderr, "etctl: no response from '%s'\n", name.c_str());
    return false;
  }
  if (*respOpcode == CTL_ERR) {
    if (!quiet) fprintf(stderr, "etctl: %s\n", respPayload->c_str());
    return false;
  }
  return true;
}

string stripAnsi(const string& in) {
  // CSI sequences, OSC sequences, and lone escapes / carriage returns.
  static const std::regex csi("\x1b\\[[0-9;?]*[ -/]*[@-~]");
  static const std::regex osc("\x1b\\][^\x07\x1b]*(\x07|\x1b\\\\)");
  static const std::regex other("\x1b[@-Z\\\\-_]");
  string s = std::regex_replace(in, osc, "");
  s = std::regex_replace(s, csi, "");
  s = std::regex_replace(s, other, "");
  s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
  return s;
}

string readAllStdin() {
  string data;
  char buf[4096];
  ssize_t rc;
  while ((rc = ::read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
    data.append(buf, rc);
  }
  return data;
}

// --- OSC 133 semantic-prompt support -------------------------------------
// The pure OSC-133 parsing (regexes + extractOsc133) lives in Osc133.hpp so it
// can be unit-tested without a pty; the detection cache and probe below add the
// stateful, I/O-bound half that only makes sense against a live session.

// Per-session cache of the resolved run framing, a sibling of the session's
// socket (~/.et/ctl/<name>.framing). Detection is done once (lazily, on the
// first run) and reused, since etctl is otherwise stateless per call. The
// stored value is the resolved RunProfile bits (see the struct below).
string framingCachePath(const string& name) {
  return control_paths::controlDir() + "/" + name + ".framing";
}
// false = no usable cache yet; otherwise the three cached bits are filled in.
// Stored as "bracket oscRead usesStatusVar" (0/1 each). A malformed or older
// (two-token) file fails to parse and reads as a miss, so a fresh open reprobes.
bool readFramingCache(const string& name, bool* bracket, bool* oscRead,
                      bool* usesStatusVar) {
  std::ifstream f(framingCachePath(name));
  if (!f.good()) return false;
  int b = -1, o = -1, s = 0;
  if (!(f >> b >> o >> s)) return false;
  if (b < 0 || o < 0) return false;
  if (bracket) *bracket = (b != 0);
  if (oscRead) *oscRead = (o != 0);
  if (usesStatusVar) *usesStatusVar = (s != 0);
  return true;
}
void writeFramingCache(const string& name, bool bracket, bool oscRead,
                       bool usesStatusVar) {
  control_paths::ensureControlDir();
  std::ofstream f(framingCachePath(name));
  if (f.good())
    f << (bracket ? 1 : 0) << " " << (oscRead ? 1 : 0) << " "
      << (usesStatusVar ? 1 : 0) << "\n";
}

// --- commands ---------------------------------------------------------------

// Quiet liveness check: a socket that accepts a connection has a live daemon.
bool sessionAlive(const string& name) {
  int fd = connectControl(name);
  if (fd < 0) {
    return false;
  }
  ::close(fd);
  return true;
}

// Block until a session stops accepting connections (its daemon has exited and
// unlinked the socket), or the timeout elapses.  Returns true if it is gone.
// This makes "end then recreate the same NAME" deterministic: teardown is
// otherwise asynchronous (the daemon may still be finishing teardown), so
// a too-soon `open` can see the still-live daemon and no-op.
bool waitSessionGone(const string& name, double secs) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds((long long)(secs * 1000));
  while (sessionAlive(name)) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    ::usleep(100 * 1000);
  }
  return true;
}

int64_t sessionField(const string& name, const string& key);  // defined below
int cmdWrite(const string& name, const string& bytes,
             bool secret);                         // defined below
int cmdKill(const string& name, double waitSecs);  // defined below

// Fetch a session's full info as a key=value map (one CTL_INFO round-trip).
std::map<string, string> sessionInfo(const string& name) {
  std::map<string, string> m;
  uint8_t op = 0;
  string payload;
  if (!oneShot(name, CTL_INFO, "", &op, &payload)) {
    return m;
  }
  std::istringstream ss(payload);
  string line;
  while (std::getline(ss, line)) {
    size_t eq = line.find('=');
    if (eq != string::npos) {
      m[line.substr(0, eq)] = line.substr(eq + 1);
    }
  }
  return m;
}

string humanizeDuration(int64_t s) {
  if (s < 0) s = 0;
  int64_t d = s / 86400;
  int64_t h = (s % 86400) / 3600;
  int64_t m = (s % 3600) / 60;
  int64_t sec = s % 60;
  char buf[64];
  if (d) {
    snprintf(buf, sizeof(buf), "%lldd%lldh", (long long)d, (long long)h);
  } else if (h) {
    snprintf(buf, sizeof(buf), "%lldh%lldm", (long long)h, (long long)m);
  } else if (m) {
    snprintf(buf, sizeof(buf), "%lldm%llds", (long long)m, (long long)sec);
  } else {
    snprintf(buf, sizeof(buf), "%llds", (long long)sec);
  }
  return string(buf);
}

// Parse a duration like "30m", "6h", "2d", "90s", or a bare number (seconds).
int64_t parseDuration(const string& s, int64_t fallback) {
  if (s.empty()) return fallback;
  char* end = nullptr;
  const double n = strtod(s.c_str(), &end);
  if (end == s.c_str()) return fallback;
  int64_t mult = 1;
  switch (*end) {
    case 'm':
      mult = 60;
      break;
    case 'h':
      mult = 3600;
      break;
    case 'd':
      mult = 86400;
      break;
    default:
      mult = 1;
      break;  // 's' or none
  }
  return (int64_t)(n * (double)mult);
}

int cmdGc(int argc, char** argv) {
  bool idle = false, force = false;
  int64_t idleSecs = 8 * 3600;  // default --idle threshold (8h)
  for (int i = 2; i < argc; i++) {
    const string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf(
          "etctl gc [--idle [DUR]] [--force]\n"
          "  Remove dead session sockets (a daemon that has exited leaves a\n"
          "  stale socket).  With --idle, also end live sessions idle longer\n"
          "  than DUR (default 8h; e.g. 30m, 6h, 2d): eof first, then a "
          "forced\n"
          "  stop if it doesn't exit within a few seconds.  --force skips the\n"
          "  graceful eof and stops idle sessions outright.\n");
      return 0;
    } else if (a == "--idle") {
      idle = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        idleSecs = parseDuration(argv[++i], idleSecs);
      }
    } else if (a.rfind("--idle=", 0) == 0) {
      idle = true;
      idleSecs = parseDuration(a.substr(strlen("--idle=")), idleSecs);
    } else if (a == "--force" || a == "--kill") {
      force = true;
    }
  }

  // First, reap live sessions idle past the threshold (so their sockets go
  // dead and get swept below). eof is graceful; fall back to a forced stop.
  if (idle) {
    const string eof(1, '\004');  // Ctrl-D ends the remote shell cleanly
    const int64_t now = (int64_t)time(NULL);
    for (const string& name : control_paths::listSessionNames()) {
      if (!sessionAlive(name)) continue;
      const int64_t last = sessionField(name, "lastActivity");
      if (last <= 0 || (now - last) < idleSecs) continue;
      if (force) {
        printf("stopping idle session: %s\n", name.c_str());
        cmdKill(name, 0.0);
      } else {
        printf("ending idle session: %s\n", name.c_str());
        cmdWrite(name, eof, false);
        if (!waitSessionGone(name, 3.0)) {
          cmdKill(name, 0.0);  // didn't exit gracefully: force-stop
        }
      }
    }
  }

  // Sweep dead sockets: originally-dangling ones plus any just reaped.
  for (const string& name : control_paths::listSessionNames()) {
    if (sessionAlive(name)) continue;
    const string path = control_paths::socketPathForName(name);
    if (::unlink(path.c_str()) == 0) {
      printf("removed dead socket: %s\n", name.c_str());
    } else if (errno != ENOENT) {
      fprintf(stderr, "etctl gc: could not remove %s: %s\n", path.c_str(),
              strerror(errno));
    }
    // Drop the session's framing detection cache along with its socket.
    ::unlink(framingCachePath(name).c_str());
  }
  return 0;
}

int cmdSessions() {
  vector<string> names = control_paths::listSessionNames();
  struct Row {
    string name, host, status, up, idle;
  };
  vector<Row> rows;
  const int64_t now = (int64_t)time(NULL);
  for (const string& name : names) {
    std::map<string, string> in = sessionInfo(name);
    if (in.empty()) {
      rows.push_back({name, "-", "dead", "-", "-"});
      continue;
    }
    const bool connected = in["connected"] == "1";
    int64_t created = in.count("created") ? atoll(in["created"].c_str()) : -1;
    int64_t last =
        in.count("lastActivity") ? atoll(in["lastActivity"].c_str()) : -1;
    rows.push_back({name,
                    in.count("host") && !in["host"].empty() ? in["host"] : "-",
                    connected ? "connected" : "disconnected",
                    created > 0 ? humanizeDuration(now - created) : "-",
                    last > 0 ? humanizeDuration(now - last) : "-"});
  }
  if (rows.empty()) {
    return 0;
  }
  // Size each column to its widest cell (header included) for a clean table.
  size_t wN = 4, wH = 4, wS = 6, wU = 2;  // NAME HOST STATUS UP
  for (const Row& r : rows) {
    wN = std::max(wN, r.name.size());
    wH = std::max(wH, r.host.size());
    wS = std::max(wS, r.status.size());
    wU = std::max(wU, r.up.size());
  }
  printf("%-*s  %-*s  %-*s  %-*s  %s\n", (int)wN, "NAME", (int)wH, "HOST",
         (int)wS, "STATUS", (int)wU, "UP", "IDLE");
  for (const Row& r : rows) {
    printf("%-*s  %-*s  %-*s  %-*s  %s\n", (int)wN, r.name.c_str(), (int)wH,
           r.host.c_str(), (int)wS, r.status.c_str(), (int)wU, r.up.c_str(),
           r.idle.c_str());
  }
  return 0;
}

int cmdInfo(const string& name) {
  uint8_t op = 0;
  string payload;
  if (!oneShot(name, CTL_INFO, "", &op, &payload)) {
    return 1;
  }
  fputs(payload.c_str(), stdout);
  return 0;
}

int cmdRead(const string& name, int64_t cursor, double timeoutSec) {
  auto emit = [&](const ScrollbackRead& r) {
    if (r.truncated) {
      fprintf(stderr,
              "etctl: warning: cursor fell behind; output gap skipped\n");
    }
    if (!r.data.empty()) {
      fwrite(r.data.data(), 1, r.data.size(), stdout);
      fflush(stdout);
    }
  };

  if (timeoutSec > 0) {
    /*
     * Wait up to timeoutSec for new output; once it starts, keep reading until
     * a brief quiet gap, then return (etch read() semantics).
     */
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds((long long)(timeoutSec * 1000));
    auto lastData = std::chrono::steady_clock::now();
    bool got = false;
    while (std::chrono::steady_clock::now() < deadline) {
      uint8_t op = 0;
      string payload;
      if (!oneShot(name, CTL_READ, control_proto::encodeCursor(cursor), &op,
                   &payload)) {
        return 1;
      }
      ScrollbackRead r = control_proto::decodeReadResp(payload);
      cursor = r.nextCursor;
      if (!r.data.empty()) {
        emit(r);
        got = true;
        lastData = std::chrono::steady_clock::now();
      } else if (got && std::chrono::steady_clock::now() - lastData >
                            std::chrono::milliseconds(300)) {
        break;  // output settled
      }
      ::usleep(50 * 1000);
    }
    fprintf(stderr, "next-cursor: %lld\n", (long long)cursor);
    return 0;
  }

  // Single non-blocking read.
  uint8_t op = 0;
  string payload;
  if (!oneShot(name, CTL_READ, control_proto::encodeCursor(cursor), &op,
               &payload)) {
    return 1;
  }
  ScrollbackRead r = control_proto::decodeReadResp(payload);
  emit(r);
  fprintf(stderr, "next-cursor: %lld\n", (long long)r.nextCursor);
  return 0;
}

int cmdWrite(const string& name, const string& bytes, bool secret = false) {
  uint8_t op = 0;
  string payload;
  if (!oneShot(name, secret ? CTL_WRITE_SECRET : CTL_WRITE, bytes, &op,
               &payload)) {
    return 1;
  }
  return 0;
}

int cmdKill(const string& name, double waitSecs) {
  uint8_t op = 0;
  string payload;
  if (!oneShot(name, CTL_KILL, "", &op, &payload)) {
    return 1;
  }
  if (waitSecs > 0 && !waitSessionGone(name, waitSecs)) {
    fprintf(stderr, "etctl: session '%s' still alive %.0fs after kill\n",
            name.c_str(), waitSecs);
    return 1;
  }
  return 0;
}

int64_t sessionField(const string& name, const string& key) {
  uint8_t op = 0;
  string payload;
  if (!oneShot(name, CTL_INFO, "", &op, &payload)) {
    return -1;
  }
  const string prefix = key + "=";
  std::istringstream ss(payload);
  string line;
  while (std::getline(ss, line)) {
    if (line.rfind(prefix, 0) == 0) {
      return atoll(line.c_str() + prefix.size());
    }
  }
  return -1;
}

int64_t sessionHeadCursor(const string& name) {
  return sessionField(name, "headCursor");
}

int cmdExpect(const string& name, const string& pattern, double timeoutSec,
              bool exact, int64_t startCursor) {
  std::regex re;
  if (!exact) {
    try {
      re = std::regex(pattern);
    } catch (const std::exception& e) {
      fprintf(stderr, "etctl: bad pattern: %s\n", e.what());
      return 2;
    }
  }
  // An explicit --cursor wins; otherwise watch for output produced from now on.
  // Capturing a cursor (info headCursor) before sending input and passing it
  // here avoids the race where the awaited text lands between the write and a
  // head-anchored expect.
  int64_t cursor = startCursor >= 0 ? startCursor : sessionHeadCursor(name);
  if (cursor < 0) cursor = 0;
  string acc;
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds((long long)(timeoutSec * 1000));
  while (std::chrono::steady_clock::now() < deadline) {
    uint8_t op = 0;
    string payload;
    if (!oneShot(name, CTL_READ, control_proto::encodeCursor(cursor), &op,
                 &payload)) {
      return 2;
    }
    ScrollbackRead r = control_proto::decodeReadResp(payload);
    cursor = r.nextCursor;
    acc += stripAnsi(r.data);
    bool matched = exact ? (acc.find(pattern) != string::npos)
                         : std::regex_search(acc, re);
    if (matched) {
      fwrite(acc.data(), 1, acc.size(), stdout);
      if (acc.empty() || acc.back() != '\n') printf("\n");
      fprintf(stderr, "next-cursor: %lld\n", (long long)cursor);
      return 0;
    }
    ::usleep(100 * 1000);
  }
  fprintf(stderr, "etctl: timed out waiting for %s%s%s\n", exact ? "\"" : "/",
          pattern.c_str(), exact ? "\"" : "/");
  return 1;
}

// How `run` injects a command and finds its output + exit code. Two orthogonal
// choices, resolved once per session by detectFraming and cached:
//   bracket -- inject the *bare* command via bracketed paste (\e[200~..\e[201~),
//              so the real command shows in scrollback with no wrapper; needs a
//              paste-aware line editor (\e[?2004h). Otherwise the command rides
//              an eval here-doc, which is POSIX and immune to its own syntax.
//   oscRead -- read the boundaries + exit code from the prompt's own OSC 133 C/D
//              marks (no injected echoes); needs an OSC 133 integration.
//              Otherwise we bracket the body with `echo <mark> .. <mark>:<code>`
//              and parse those, which works on any line shell.
// The cleanest combo (bracket+oscRead) needs both; the universal fallback
// (neither) works anywhere a POSIX here-doc does.
struct RunProfile {
  bool bracket = false;
  bool oscRead = false;
  // fish and zsh accept $status for the exit code; bash/sh/dash need $?.
  bool usesStatusVar = false;
};

// `run`'s framing override: auto-detect (the default), or force one extreme --
// the cleanest bracketed+OSC path, or the universal here-doc + echo markers.
enum class RunOverride { kAuto, kForceBracketOsc, kForceMark };

// Read scrollback from `startCursor`, appending each chunk, until `stop(acc,
// grew)` returns true or the deadline passes. `grew` says whether the last read
// advanced the cursor; a stall (no new bytes) sleeps briefly. This is the shared
// core of every scrollback poll below.
template <typename Stop>
string readScroll(const string& name, int64_t startCursor, double timeoutSec,
                  Stop stop) {
  int64_t cursor = startCursor < 0 ? 0 : startCursor;
  string acc;
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds((long long)(timeoutSec * 1000));
  while (std::chrono::steady_clock::now() < deadline) {
    uint8_t op = 0;
    string payload;
    if (!oneShot(name, CTL_READ, control_proto::encodeCursor(cursor), &op,
                 &payload))
      break;
    ScrollbackRead r = control_proto::decodeReadResp(payload);
    const bool grew = (r.nextCursor != cursor);
    cursor = r.nextCursor;
    acc += r.data;
    if (stop(acc, grew)) break;
    if (!grew) ::usleep(40 * 1000);
  }
  return acc;
}

// Send `framed`, then read until this probe's OSC 133 D mark arrives (plus a
// beat to catch the next prompt's paste toggle), or a short grace after its
// sentinel has echoed twice (input + output => it ran), or the deadline.
// Captures from a fresh cursor so back-to-back probes don't see each other.
string probeRead(const string& name, const string& framed,
                 const string& sentinel, double timeoutSec) {
  const int64_t start = sessionHeadCursor(name);
  if (cmdWrite(name, framed) != 0) return "";
  const std::regex sentRe(sentinel);
  bool sawD = false, graced = false;
  auto until = std::chrono::steady_clock::now();
  return readScroll(name, start, timeoutSec, [&](const string& acc, bool) {
    const auto now = std::chrono::steady_clock::now();
    if (!sawD && std::regex_search(acc, kOsc133D)) {
      sawD = true;  // read a beat past D for the next prompt's paste toggle
      until = now + std::chrono::milliseconds(400);
    }
    if (sawD) return now >= until;
    if (!graced &&
        std::distance(std::sregex_iterator(acc.begin(), acc.end(), sentRe),
                      std::sregex_iterator()) >= 2) {
      graced = true;  // echoed input + printed output => command ran
      until = now + std::chrono::milliseconds(600);
    }
    return graced && now >= until;
  });
}

// Read the connect handshake the open preamble leaves in scrollback. Its last
// command is `echo "ETCTL_EC=$status"`, so a fresh session already carries the
// exit-code capability marker and, in the prompt right after it, the framing
// marks. Reads from the oldest retained byte (the handshake is at the very start
// of the session), drains what is there, and -- if the marker hasn't landed yet
// (first run racing the connect) -- waits for it plus a short beat for the
// following prompt. Returns the accumulated bytes; an absent marker (older
// etctl, or the handshake scrolled away) tells the caller to fall back.
string readConnectHandshake(const string& name, double timeoutSec) {
  const std::regex marker("ETCTL_EC=[0-9]*[\r\n]");
  bool sawMarker = false;
  auto beatEnd = std::chrono::steady_clock::now();
  // Oldest retained: the handshake is at the very start of the session.
  return readScroll(name, 0, timeoutSec, [&](const string& acc, bool grew) {
    const auto now = std::chrono::steady_clock::now();
    if (!sawMarker && std::regex_search(acc, marker)) {
      sawMarker = true;
      beatEnd = now + std::chrono::milliseconds(300);
    }
    // Drained after the marker => the following prompt is already captured.
    if (sawMarker) return !grew || now >= beatEnd;
    return acc.size() > 65536;  // scanned a big window, no marker: it isn't there
  });
}

// Detect the cleanest framing the session's prompt supports, ideally without
// sending anything. The open preamble ends with `echo "ETCTL_EC=$status"`, so a
// fresh session's scrollback already holds both signals: the exit-code
// capability ($status expands to a digit in fish/zsh, empty in bash/sh) and, in
// the prompt right after it, the framing marks (OSC-133 D, bracketed-paste
// ?2004). Reading those back is a zero-probe first run. Only when the marker is
// absent (older etctl, or the handshake scrolled away) do we fall back to a
// single probe. Run once per session by runCommand and cached.
RunProfile detectFraming(const string& name, double timeoutSec) {
  RunProfile prof;

  // Primary: deduce everything from the connect handshake already in scrollback.
  {
    const string acc = readConnectHandshake(name, timeoutSec);
    if (std::regex_search(acc, std::regex("ETCTL_EC=[0-9]*[\r\n]"))) {
      prof.usesStatusVar =
          std::regex_search(acc, std::regex("ETCTL_EC=[0-9]"));
      prof.bracket = acc.find("\x1b[?2004") != string::npos;
      prof.oscRead = std::regex_search(acc, kOsc133D);
      return prof;
    }
  }

  // Fallback: no connect marker. Defang `!` history expansion (the older connect
  // preamble that would have done so is absent), then send one probe that
  // doubles as the framing sentinel and the $status capability check.
  cmdWrite(name, "set +o histexpand 2>/dev/null\n");
  std::random_device rd;
  static const char* kHex = "0123456789abcdef";
  string tag;
  for (int i = 0; i < 8; i++) tag.push_back(kHex[rd() % 16]);
  const string sentinel = "ETCTL_OSCPROBE_" + tag;
  const string acc = probeRead(name, "echo \"" + sentinel + "=$status\"\n",
                               sentinel, timeoutSec);
  prof.usesStatusVar =
      std::regex_search(acc, std::regex(sentinel + "=[0-9]"));
  prof.bracket = acc.find("\x1b[?2004") != string::npos;
  prof.oscRead = std::regex_search(acc, kOsc133D);
  return prof;
}

// The state of the far-side prompt just before a command is injected: where to
// start capturing, and whether bracketed paste is armed there.
struct PromptSnapshot {
  int64_t cursor = 0;   // capture everything at or after this offset
  bool pasteOn = true;  // is bracketed paste armed at the live prompt?
};

// Wait for the session to go quiet, then snapshot the capture cursor and the
// live paste state together, from the same drained window.
//
// The settling is not politeness, it is correctness. `run` frames a command as
// "everything after this cursor", so any byte still in flight from the previous
// command lands on the wrong side of it and is read as part of *this* one. The
// byte that matters is the OSC-133 D that precmd emits for the previous line:
// snapshot too early and the capture opens with an orphan done-mark, which is
// exactly what a line we just interrupted leaves behind ("D;130"). Draining to
// quiescence first keeps that noise where it belongs -- behind the cursor.
//
// Paste state comes from the same window: an idle prompt re-arms paste
// (\e[?2004h) after each command, while a full-screen app or a redraw can leave
// it off (\e[?2004l). Only a trailing ?2004l counts as off; with no toggle in
// the window we assume on (the prompt enabled it earlier), so bracket falls
// back to the here-doc only on positive evidence that paste is currently off.
//
// `quietMs` is how long the far side must stay silent to count as settled, and
// `awaitActivity` makes the wait start only once something new has arrived --
// needed right after sending a Ctrl-C, where the shell has not yet had a round
// trip to react and "silent" would otherwise mean "hasn't heard us yet".
//
// Bounded: a session that never goes quiet (a background job writing to the
// tty) gets `maxSec` and then we proceed with what we have.
PromptSnapshot snapshotPrompt(const string& name, double maxSec,
                              int quietMs = 60, bool awaitActivity = false) {
  PromptSnapshot snap;
  const int64_t head = sessionHeadCursor(name);
  if (head < 0) return snap;
  // Read a trailing ~8 KB window so the last paste toggle is in view. That
  // first read is history, not activity, so it must not restart the quiet
  // clock: on an already-idle prompt this settles in one extra poll.
  int64_t cursor = head > 8192 ? head - 8192 : 0;
  string tail;
  bool seeded = false, sawActivity = false;
  const auto quiet = std::chrono::milliseconds(quietMs);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds((long long)(maxSec * 1000));
  auto quietSince = std::chrono::steady_clock::now();
  while (true) {
    uint8_t op = 0;
    string payload;
    if (!oneShot(name, CTL_READ, control_proto::encodeCursor(cursor), &op,
                 &payload))
      break;
    ScrollbackRead r = control_proto::decodeReadResp(payload);
    const bool grew = (r.nextCursor != cursor);
    cursor = r.nextCursor;
    tail += r.data;
    const auto now = std::chrono::steady_clock::now();
    if (grew && seeded) {
      sawActivity = true;
      quietSince = now;
    } else if (now - quietSince >= quiet && (sawActivity || !awaitActivity)) {
      break;  // the prompt has stopped moving
    }
    seeded = true;
    if (now >= deadline) break;
    ::usleep(20 * 1000);
  }
  snap.cursor = cursor;
  const size_t on = tail.rfind("\x1b[?2004h");
  const size_t off = tail.rfind("\x1b[?2004l");
  snap.pasteOn = (off == string::npos) || (on != string::npos && on > off);
  return snap;
}

/*
 * run(): send a command and collect its output verbatim + real exit code.
 * The framing is the 2x2 of two orthogonal axes (see RunProfile), cheapest
 * scrollback first:
 *  - bracket + oscRead types the *bare* command inside bracketed paste, so the
 *    real command shows in the scrollback with no wrapper and no markers, and
 *    reads the boundaries + exit code from the prompt's OSC 133 C/D marks.
 *  - bracket alone also pastes the *bare* command (so it is fish-safe: no eval
 *    here-doc, which fish cannot parse), but wraps it in `echo <mark> ...
 *    <mark>:<status>` and parses those -- for a bracketed-paste shell with no
 *    OSC 133 (e.g. default fish).
 *  - oscRead alone injects the eval here-doc (below) and reads OSC 133 -- for an
 *    OSC 133 prompt whose line editor lacks bracketed paste.
 *  - neither injects the eval here-doc bracketed by `echo <mark> ... <mark>:$?`
 *    and parses those -- the POSIX fallback for a shell with neither OSC 133
 *    nor bracketed paste (e.g. dash).
 * The eval here-doc hands the body to `eval` as *data*, never source the line
 * reader parses, so it is syntactically complete and immune to the body's own
 * quotes/braces/`!`/parse errors; bracketed paste gets the same multi-line and
 * metacharacter safety from the paste, minus a here-doc's collision-proofing
 * (so a body literally containing the paste terminator falls back to eval).
 * If `bodyOut` is non-null the body is captured there; else it goes to stdout.
 */
int runCommand(const string& name, const string& command, double timeoutSec,
               string* bodyOut, RunOverride ovr = RunOverride::kAuto) {
  // Empty / whitespace-only body: a no-op. Short-circuit so bracketed paste
  // does not submit a blank line (which runs nothing and emits no C/D mark).
  if (command.find_first_not_of(" \t\r\n") == string::npos) {
    if (bodyOut) bodyOut->clear();
    return 0;
  }

  // Resolve kAuto once, using the per-session cache; detect lazily on the first
  // run (etctl is stateless per call, so the result is cached beside the
  // socket).
  RunProfile prof;
  if (ovr == RunOverride::kForceBracketOsc) {
    prof.bracket = true;
    prof.oscRead = true;
  } else if (ovr == RunOverride::kForceMark) {
    // leave both false: the universal here-doc + echo markers.
  } else if (!readFramingCache(name, &prof.bracket, &prof.oscRead,
                               &prof.usesStatusVar)) {
    // kAuto, cache miss: detect once (lazily) and cache beside the socket.
    prof = detectFraming(name, 3.0);
    writeFramingCache(name, prof.bracket, prof.oscRead, prof.usesStatusVar);
  }

  const bool oscRead = prof.oscRead;
  // Settle the prompt, then take the capture cursor and the live paste state
  // from that same quiet moment (see snapshotPrompt: a cursor sampled while the
  // previous command is still draining captures its trailing marks as if they
  // were ours).
  const PromptSnapshot snap = snapshotPrompt(name, 2.0);
  // Inject bare via bracketed paste only when the prompt supports it, the body
  // cannot collide with the paste terminator, AND paste is enabled at the live
  // prompt right now (a full-screen app or a redraw may have turned it off).
  // Otherwise fall back to the eval here-doc, which needs neither.
  const bool useBracket =
      prof.bracket && command.find("\x1b[201~") == string::npos && snap.pasteOn;
  // fish and zsh accept $status for the exit code; bash/sh/dash need $?.
  const string statusVar = prof.usesStatusVar ? "$status" : "$?";

  string tag;
  std::random_device rd;
  static const char* kHex = "0123456789abcdef";
  for (int i = 0; i < 8; i++) tag.push_back(kHex[rd() % 16]);
  const string bodyMark = "ETCTL_BODY_" + tag;  // here-doc delimiter
  const string mark = "ETCTL_" + tag;           // echo-marker (non-OSC framings)

  int64_t cursor = snap.cursor;
  if (cursor < 0) cursor = 0;

  // The 2x2 of the two axes: inject via paste vs eval here-doc, and read the
  // exit code from OSC 133 vs from our own echo-markers.
  string framed;
  if (useBracket && oscRead) {
    framed = "\x1b[200~" + command + "\x1b[201~\r";
  } else if (useBracket) {
    // Bare body via bracketed paste (fish-safe: no here-doc), wrapped in
    // echo-markers since there are no OSC 133 marks to read.
    framed = "\x1b[200~echo " + mark + "\n" + command + "\necho " + mark + ":" +
             statusVar + "\x1b[201~\r";
  } else if (oscRead) {
    framed = "eval \"$(cat <<'" + bodyMark + "'\n" + command + "\n" + bodyMark +
             "\n)\"\n";
  } else {
    framed = "echo " + mark + "; eval \"$(cat <<'" + bodyMark + "'\n" +
             command + "\n" + bodyMark + "\n)\"; echo " + mark + ":" +
             statusVar + "\n";
  }
  if (cmdWrite(name, framed) != 0) {
    return 2;
  }

  // kMark markers: the end marker "<mark>:<code>\n" is anchored on the trailing
  // newline so a chunked read never matches a truncated code, and the start
  // marker "<mark>\r?\n" ignores the echoed command and zsh's OSC window-title
  // (both of which also contain <mark>). Unused by the OSC framings.
  std::regex endRe(mark + ":([0-9]+)\\r?\\n");
  std::regex startRe(mark + "\\r?\\n");
  std::regex execStartRe("[\\r\\n]" + mark + "\\r?\\n");
  string acc;
  const auto typedAt = std::chrono::steady_clock::now();
  const auto deadline =
      typedAt + std::chrono::milliseconds((long long)(timeoutSec * 1000));
  bool accepted = false;
  while (std::chrono::steady_clock::now() < deadline) {
    uint8_t op = 0;
    string payload;
    if (!oneShot(name, CTL_READ, control_proto::encodeCursor(cursor), &op,
                 &payload)) {
      return 2;
    }
    ScrollbackRead r = control_proto::decodeReadResp(payload);
    cursor = r.nextCursor;
    acc += r.data;

    string body;
    int code = 0;
    bool done = false;
    if (oscRead) {
      done = extractOsc133(acc, &body, &code);
    } else {
      std::smatch m;
      if (std::regex_search(acc, m, endRe)) {
        code = atoi(m[1].str().c_str());
        size_t endPos = (size_t)m.position(0);
        // Take the LAST start marker before the end marker. A bracketed paste
        // (kBracketMark) echoes the pasted `echo <mark>` line before executing
        // it, so the first match is the echo; the executed one is the last.
        size_t bodyStart = 0;
        for (auto it = std::sregex_iterator(acc.begin(), acc.end(), startRe),
                  se = std::sregex_iterator();
             it != se; ++it) {
          size_t after = (size_t)it->position(0) + (size_t)it->length(0);
          if (after <= endPos)
            bodyStart = after;
          else
            break;
        }
        body = acc.substr(bodyStart, endPos - bodyStart);
        done = true;
      }
    }
    if (done) {
      if (bodyOut) {
        *bodyOut = body;
      } else {
        fwrite(body.data(), 1, body.size(), stdout);
      }
      return code;
    }
    // Continuation guard (bracketed inject only): if the pasted body is
    // malformed (unbalanced quote, incomplete construct) the shell parks on a
    // continuation prompt and our end marker never comes. Detect that and
    // Ctrl-C to recover, rather than hang to the full timeout.
    //
    // The signal is positive evidence that the shell *re-prompted without
    // running anything*: zle drops bracketed paste (\e[?2004l) to evaluate what
    // it received, then re-arms it (\e[?2004h) to read the continuation. A
    // command that is actually executing leaves paste off for its whole
    // duration, so this cannot fire on a slow command -- and, unlike a
    // deadline, it cannot fire on a slow *link* either. Absence of a mark is
    // never enough on its own: a late C is indistinguishable from a parked
    // shell, and acting on that guess is what discarded the output of commands
    // that had already run.
    if (useBracket) {
      bool parked = false;
      const size_t pasteOff = acc.find("\x1b[?2004l");
      const bool rearmed = pasteOff != string::npos &&
                           acc.find("\x1b[?2004h", pasteOff) != string::npos;
      if (oscRead) {
        // Either mark proves the line was accepted: C is emitted by preexec the
        // instant it runs, D by precmd when it finishes. Once accepted, never
        // interrupt -- whatever is missing, the command is the shell's now.
        if (std::regex_search(acc, kOsc133C) ||
            std::regex_search(acc, kOsc133D))
          accepted = true;
        parked = !accepted && rearmed;
      } else {
        // kBracketMark has no C mark, and two shells park two ways:
        //  - bash runs the leading marker line then parks on PS2, toggling
        //    paste off (?2004l) then back on (?2004h): that re-arm, with no
        //    end marker, means it re-prompted = parked (fast).
        //  - fish parks atomically, running nothing and never toggling paste:
        //    no executed start marker (mark at column 0, vs the echoed
        //    `echo <mark>`) after a short grace = parked.
        // A still-running command keeps paste disabled and has already emitted
        // the executed marker, so neither branch fires: it is never
        // interrupted.
        if (rearmed) {
          parked = true;
        } else if (std::chrono::steady_clock::now() - typedAt >
                       std::chrono::milliseconds(2000) &&
                   !std::regex_search(acc, execStartRe)) {
          parked = true;
        }
      }
      if (parked) {
        cmdWrite(name, "\x03");  // Ctrl-C to abort the continuation prompt
        // Let the interrupt land and the shell re-prompt before returning. The
        // D that precmd emits for the aborted line has to be in the scrollback
        // *before* the next run snapshots its cursor, or that run opens its
        // capture on an orphan done-mark. Wait for the shell to actually react
        // (a round trip away), not merely for the line to be quiet.
        snapshotPrompt(name, 2.0, 200, /*awaitActivity=*/true);
        fprintf(stderr,
                "etctl: command looks incomplete (shell parked on a "
                "continuation prompt); aborted\n");
        return 125;
      }
    }
    ::usleep(80 * 1000);
  }
  // A bracketed inject that never completed may have left the shell parked on a
  // continuation prompt; recover it, and drain the aftermath so the next run is
  // not handed an orphan done-mark.
  if (useBracket) {
    cmdWrite(name, "\x03");
    snapshotPrompt(name, 2.0, 200, /*awaitActivity=*/true);
  }
  fprintf(stderr, "etctl: run timed out after %.1fs\n", timeoutSec);
  return 124;
}

int cmdRun(const string& name, const string& command, double timeoutSec,
           RunOverride ovr = RunOverride::kAuto) {
  return runCommand(name, command, timeoutSec, nullptr, ovr);
}

int cmdOpen(int argc, char** argv) {
  /*
   * etctl open NAME [et-args...]  ->  et --ctl --attach NAME [et-args...]
   *
   * NAME is a positional (consistent with the other verbs); etctl owns it and
   * hands it to et as --attach, which both names the session and adopts one
   * already running under that name.  The call is idempotent at two levels: a
   * live session short-circuits below, and a session whose *client* died is
   * adopted rather than duplicated.  That second case is the one that used to
   * strand sessions: the socket is gone, so the checks below see nothing and a
   * plain open would bootstrap a second session over SSH while the first kept
   * running on the host, holding its shell, cwd and jobs with nothing able to
   * reach them.  Either way `open` is a cheap "ensure this session exists" step
   * you can safely run before driving it, which is the clean version of etch's
   * autospawn -- the connection details live only here, not on every command.
   */
  if (argc < 3) {
    fprintf(stderr,
            "etctl open: NAME required (etctl open NAME [et-args...])\n");
    return 2;
  }
  string name = argv[2];

  // Respect a custom socket override for the liveness check, and note the
  // requested destination (the trailing positional) so we can guard against
  // reusing a name that points at a different host.
  string checkTarget = name;
  string requestedDest;
  string userCommand;
  vector<bool> skip(argc, false);
  for (int i = 3; i < argc; i++) {
    string a = argv[i];
    if (a == "--ctl-socket" && i + 1 < argc) {
      checkTarget = argv[i + 1];
      i++;
    } else if (a.rfind("--ctl-socket=", 0) == 0) {
      checkTarget = a.substr(strlen("--ctl-socket="));
    } else if (a == "--attach" && i + 1 < argc) {
      i++;  // skip et's --attach value, it isn't the destination
    } else if ((a == "-c" || a == "--command") && i + 1 < argc) {
      // Pull out a user-supplied connect command so we can merge it with our
      // own setup (below) rather than fight over et's single -c.  Marking its
      // tokens to skip also keeps the value from being mistaken for the
      // destination by the bare-positional branch.
      userCommand = argv[i + 1];
      skip[i] = true;
      skip[i + 1] = true;
      i++;
    } else if (a.rfind("--command=", 0) == 0) {
      userCommand = a.substr(strlen("--command="));
      skip[i] = true;
    } else if (!a.empty() && a[0] != '-') {
      requestedDest = a;  // last bare positional wins (et's destination)
    }
  }
  if (sessionAlive(checkTarget)) {
    // Reusing a live session is the point of idempotent open -- but only if it
    // is the same host.  A name pointing at a different host is almost always a
    // collision (two actors picked the same name), so fail loudly instead of
    // silently driving the wrong box.
    const string existingHost = sessionInfo(checkTarget)["host"];
    auto hostPart = [](string s) {
      size_t at = s.find('@');
      if (at != string::npos) s = s.substr(at + 1);
      size_t colon = s.find(':');
      if (colon != string::npos) s = s.substr(0, colon);
      return s;
    };
    const bool looksLikeDest = requestedDest.find('@') != string::npos ||
                               requestedDest.find('.') != string::npos;
    if (looksLikeDest && !existingHost.empty() &&
        hostPart(requestedDest) != hostPart(existingHost)) {
      fprintf(stderr,
              "etctl: session '%s' is connected to %s, not %s -- pick a "
              "different name (e.g. add a unique suffix)\n",
              name.c_str(), existingHost.c_str(), requestedDest.c_str());
      return 2;
    }
    fprintf(stderr, "etctl: session '%s' already running (%s)\n", name.c_str(),
            existingHost.empty() ? "?" : existingHost.c_str());
    return 0;
  }

  // Establishing a fresh session under this name: drop any stale framing
  // detection a previous session left behind, so the first run re-probes.
  ::unlink(framingCachePath(name).c_str());

  string etPath = "et";
  string self = argv[0];
  size_t slash = self.find_last_of('/');
  if (slash != string::npos) {
    string sibling = self.substr(0, slash + 1) + "et";
    if (::access(sibling.c_str(), X_OK) == 0) {
      etPath = sibling;  // prefer the et next to this etctl (dev/build trees)
    }
  }
  // The connect command runs once in the persistent --ctl shell; we use it for
  // the whole session handshake:
  //  - `set +o histexpand` disables `!` history expansion (a scripted session
  //    never wants it; the eval here-doc is immune, bracketed paste is not). It
  //    hits the option natively in both bash and zsh; sh/fish have no `!`
  //    expansion and simply error into /dev/null.
  //  - ETCTL_SESSION stamps the session so the remote shell/prompt/scripts know
  //    they are being driven, and which session.
  //  - a trailing `echo "ETCTL_EC=$status"` emits the exit-code capability
  //    marker ($status -> a digit in fish/zsh, empty in bash/sh) so the first
  //    run can deduce the framing (from the following prompt) and the marker's
  //    status var straight from scrollback, without sending a probe.
  // A user-supplied -c is merged in the middle.
  string setupCommand =
      "set +o histexpand 2>/dev/null; export ETCTL_SESSION='" + name + "'";
  if (!userCommand.empty()) {
    setupCommand += "; " + userCommand;
  }
  setupCommand += "; echo \"ETCTL_EC=$status\"";

  vector<char*> args;
  args.push_back(strdup(etPath.c_str()));
  args.push_back(strdup("--ctl"));
  args.push_back(strdup("--attach"));
  args.push_back(strdup(name.c_str()));
  for (int i = 3; i < argc; i++) {
    if (skip[i]) continue;  // user's -c is folded into setupCommand below
    args.push_back(strdup(argv[i]));
  }
  args.push_back(strdup("--command"));
  args.push_back(strdup(setupCommand.c_str()));
  args.push_back(nullptr);
  execvp(args[0], args.data());
  fprintf(stderr, "etctl open: could not exec et (%s): %s\n", etPath.c_str(),
          strerror(errno));
  return 127;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printOverview();
    return 0;
  }
  string cmd = argv[1];

  // Make Ctrl-C exit cleanly instead of tripping easyloggingpp's crash handler.
  installInterruptHandler();

  if (cmd == "-h" || cmd == "--help") {
    printOverview();
    return 0;
  }
  if (cmd == "-v" || cmd == "--version" || cmd == "version") {
    printf("etctl version %s\n", ET_VERSION);
    return 0;
  }
  if (cmd == "help") {
    if (argc > 2 && !descFor(argv[2]).empty()) {
      fputs(buildOptions(argv[2]).help({""}).c_str(), stdout);
    } else {
      printOverview();
    }
    return 0;
  }
  if (cmd == "open") {
    for (int i = 2; i < argc; i++) {
      string a = argv[i];
      if (a == "-h" || a == "--help") {
        // open parses argv itself (NAME + et passthrough), so build its cxxopts
        // Options inline to render descFor + Usage + -h exactly as the other
        // verbs do; the `...` passthrough (not a real option) is appended in
        // the same column so the options block reads as one.
        cxxopts::Options opts("etctl open", descFor("open"));
        opts.add_options()("h,help", "Print help");
        opts.positional_help("");
        opts.custom_help("NAME [OPTION...] [user@]host[:port]");
        fputs(opts.help({""}).c_str(), stdout);
        printf(
            "  ...         Passed directly to et (see `et --help`)\n"
            "\n"
            "  NAME is the session name (etctl supplies et's --attach);\n"
            "  if NAME is already running it does nothing.\n");
        return 0;
      }
    }
    return cmdOpen(argc, argv);
  }
  if (cmd == "gc") {
    return cmdGc(argc, argv);
  }
  if (descFor(cmd).empty()) {
    fprintf(stderr, "etctl: unknown command '%s'\n", cmd.c_str());
    printOverview();
    return 2;
  }

  cxxopts::Options opts = buildOptions(cmd);
  cxxopts::ParseResult res;
  try {
    res = opts.parse(argc - 1, argv + 1);
  } catch (const std::exception& e) {
    fprintf(stderr, "etctl %s: %s\n", cmd.c_str(), e.what());
    return 2;
  }
  if (res.count("help")) {
    fputs(opts.help({""}).c_str(), stdout);
    return 0;
  }
  if (cmd == "sessions") return cmdSessions();

  if (!res.count("NAME")) {
    fprintf(stderr, "etctl %s: missing session NAME\n", cmd.c_str());
    return 2;
  }
  string name = res["NAME"].as<string>();

  if (cmd == "info") return cmdInfo(name);
  if (cmd == "kill")
    return cmdKill(name, res.count("wait") ? res["wait"].as<double>() : 0.0);

  if (cmd == "read") {
    int64_t cursor = res.count("cursor") ? res["cursor"].as<long long>() : -1;
    double timeout = res.count("timeout") ? res["timeout"].as<double>() : 0.0;
    return cmdRead(name, cursor, timeout);
  }
  if (cmd == "write") {
    // TEXT arg if given, else stdin; raw bytes, no trailing newline.  A hidden
    // secret belongs on writeln (a password needs the submitting newline).
    string bytes =
        res.count("TEXT") ? res["TEXT"].as<string>() : readAllStdin();
    return cmdWrite(name, bytes);
  }
  if (cmd == "writeln") {
    string text = res.count("TEXT") ? res["TEXT"].as<string>() : string();
    bool secret = res.count("secret") > 0;
    if (secret) {
      char* pw = getpass("input (hidden): ");
      text = pw ? string(pw) : string();
    }
    return cmdWrite(name, text + "\n", secret);
  }
  if (cmd == "run") {
    if (!res.count("CMD")) {
      fprintf(stderr, "etctl run: missing CMD\n");
      return 2;
    }
    RunOverride ovr = RunOverride::kAuto;
    const string framing = res["framing"].as<string>();
    if (framing == "osc133") {
      ovr = RunOverride::kForceBracketOsc;
    } else if (framing == "mark") {
      ovr = RunOverride::kForceMark;
    } else if (framing != "auto") {
      fprintf(stderr,
              "etctl run: unknown --framing '%s' (want auto|osc133|mark)\n",
              framing.c_str());
      return 2;
    }
    return cmdRun(name, res["CMD"].as<string>(), res["timeout"].as<double>(),
                  ovr);
  }
  if (cmd == "expect") {
    if (!res.count("PATTERN")) {
      fprintf(stderr, "etctl expect: missing PATTERN\n");
      return 2;
    }
    return cmdExpect(name, res["PATTERN"].as<string>(),
                     res["timeout"].as<double>(), res.count("exact") > 0,
                     res.count("cursor") ? res["cursor"].as<long long>() : -1);
  }

  fprintf(stderr, "etctl: unhandled command '%s'\n", cmd.c_str());
  return 2;
}
