/*
 * etctl: the client-side control CLI for backgrounded `et --ctl` sessions.
 *
 * It is a thin, stateless translator: each invocation resolves a session's local
 * control socket (~/.et/ctl/<name>.sock), sends one native control frame, and
 * prints the response.  The transport carries ET's own vocabulary (raw input
 * bytes and scrollback reads); the verbs here are ergonomic sugar composed from it.
 */
#include <algorithm>
#include <csignal>
#include <map>
#include <regex>
#include <sstream>

#include <cxxopts.hpp>

#include "ControlPaths.hpp"
#include "ControlProtocol.hpp"
#include "ETerminal.pb.h"
#include "Headers.hpp"

using namespace et;

namespace {

/*
 * Ctrl-C handling.  et-lib's easyloggingpp installs a crash handler that catches
 * SIGINT and aborts with a "CRASH HANDLED" backtrace; but here Ctrl-C is the
 * normal way to stop a blocking verb (run, expect, wait, read --follow, peep
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
  if (cmd == "info") return "Show session status (liveness, link, size, cursor).";
  if (cmd == "kill") return "Force-stop a session's local daemon.";
  if (cmd == "read") return "Read session output without consuming it.";
  if (cmd == "write") return "Inject raw input bytes (a TEXT arg, or stdin).";
  if (cmd == "writeln")
    return "Inject a line of input (or a hidden password).";
  if (cmd == "run") return "Run a command; print its output verbatim, exit with its code.";
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
    o.add_options()
        ("cursor", "Start at byte offset N (default: oldest retained)",
         cxxopts::value<long long>())
        ("timeout", "Wait up to S seconds for new output, then return",
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
    pos("NAME", "session", cxxopts::value<string>())(
        "TEXT", "text to send", cxxopts::value<string>());
    o.parse_positional({"NAME", "TEXT"});
    synopsis = "NAME [TEXT] [--secret]";
  } else if (cmd == "run") {
    o.add_options()("timeout", "Seconds before giving up",
                    cxxopts::value<double>()->default_value("60"));
    pos("NAME", "session", cxxopts::value<string>())(
        "CMD", "command to run", cxxopts::value<string>());
    o.parse_positional({"NAME", "CMD"});
    synopsis = "NAME CMD [OPTION...]";
  } else if (cmd == "expect") {
    o.add_options()
        ("timeout", "Seconds before giving up",
         cxxopts::value<double>()->default_value("30"))
        ("exact", "Match PATTERN as a literal substring, not a regex")
        ("cursor", "Scan output from byte offset N (capture it before writing "
                   "to avoid races)",
         cxxopts::value<long long>());
    pos("NAME", "session", cxxopts::value<string>())(
        "PATTERN", "regex (or literal with --exact)", cxxopts::value<string>());
    o.parse_positional({"NAME", "PATTERN"});
    synopsis = "NAME PATTERN [OPTION...]";
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
          "  NAME is a session name (under ~/.et/ctl, or $ETCTL_HOME) or a "
          "socket path.\n"
          "\n"
          "  open        start a control session in the background (idempotent)\n"
          "  run         run a command; capture its verbatim output + exit code\n"
          "  read        read output (non-destructive)\n"
          "  write       inject raw input bytes (no newline)\n"
          "  writeln     inject a line (or a hidden password)\n"
          "  expect      wait for a pattern in the output\n"
          "  info        show session status\n"
          "  sessions    list local control sessions\n"
          "  kill        force-stop a session daemon\n");
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
    if (!quiet)
      fprintf(stderr, "etctl: cannot reach session '%s' (%s)\n", name.c_str(),
              strerror(errno));
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
  const auto deadline =
      std::chrono::steady_clock::now() +
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
    int64_t last = in.count("lastActivity") ? atoll(in["lastActivity"].c_str()) : -1;
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
  // Capturing a cursor (info headCursor) before sending input and passing it here
  // avoids the race where the awaited text lands between the write and a
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

/*
 * run(): send a command and collect its output verbatim + real exit code, the way
 * etch.run does.  We frame the command with unique start/end markers (echoed by the
 * shell) and parse the exit code printed after it; the body between the markers is
 * passed through untouched -- ANSI colors, control bytes, and the pty's own CR all
 * survive, so `run` is an 8-bit-clean pipe.  This assumes a cooperating line-oriented
 * shell on the far side.  If `bodyOut` is non-null the body is captured there;
 * otherwise it is written to stdout.  (Validated against a live et session, not the
 * in-process echo harness, which does not execute commands.)
 */
int runCommand(const string& name, const string& command, double timeoutSec,
               string* bodyOut) {
  string tag;
  std::random_device rd;
  static const char* kHex = "0123456789abcdef";
  for (int i = 0; i < 8; i++) tag.push_back(kHex[rd() % 16]);
  const string mark = "ETCTL_" + tag;
  // Here-doc delimiter for the body, kept distinct from `mark` (a different
  // prefix, not just a different suffix) so neither marker regex can ever
  // match the delimiter line that the pty echoes back.
  const string bodyMark = "ETCTL_BODY_" + tag;

  int64_t cursor = sessionHeadCursor(name);
  if (cursor < 0) cursor = 0;

  // Echo the start marker, eval the body, echo the end marker + $?.  The body
  // is captured by `cat` from a *quoted* here-doc and handed to `eval` as a
  // string -- i.e. data, never source the interactive line reader parses.  So
  // the control line we type is always syntactically complete and carries none
  // of the body's own syntax:
  //   - a parse error in the body fails at eval *runtime* -- the end marker
  //     still prints (non-zero code) instead of desyncing the frame, no hang;
  //   - history expansion ('!') and glob/brace/quote metacharacters are inert,
  //     since a quoted here-doc suppresses all expansion of its content (a bare
  //     '!.]'-style glob used to trip zsh's "event not found").
  // `eval` runs in the current shell, so cd/export still persist across runs.
  // We use `eval "$(cat <<...)"` rather than `. /dev/fd/N` because it needs no
  // /dev/fd entry (absent on e.g. FreeBSD without fdescfs) and keeps the body's
  // stdin (fd 0) on the pty: `cat` reads the here-doc in the command-sub
  // subshell, never touching the outer shell's fd 0.  The open '$(' + here-doc
  // also keeps the first line incomplete, so a line-editing shell waits for the
  // whole body.
  string framed = "echo " + mark + "; eval \"$(cat <<'" + bodyMark + "'\n" +
                  command + "\n" + bodyMark + "\n)\"; echo " + mark + ":$?\n";
  if (cmdWrite(name, framed) != 0) {
    return 2;
  }

  // The end marker is "<mark>:<code>\n". Anchoring on the trailing newline
  // (\r? tolerates the PTY's ONLCR) ensures we only match once the *whole* exit
  // code has arrived; without it, a chunked read could match a truncated code.
  // It also skips the echoed command and zsh's OSC window-title (both contain
  // "<mark>:$?" -- no digit after ':' -- which never matches here).
  std::regex endRe(mark + ":([0-9]+)\\r?\\n");
  // The start marker is the line the start echo prints, "<mark>\r?\n".  <mark>
  // also appears earlier -- in the echoed command and in zsh's OSC window-title
  // escape ("\e]2;echo <mark>...\a") -- but only the real output line has <mark>
  // immediately followed by a newline, so anchor on that (a bare find would land
  // in the title; the colored echo splits <mark> per character and never matches).
  std::regex startRe(mark + "\\r?\\n");
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
    acc += r.data;

    std::smatch m;
    if (std::regex_search(acc, m, endRe)) {
      int code = atoi(m[1].str().c_str());
      size_t endPos = (size_t)m.position(0);
      std::smatch sm;
      size_t bodyStart = 0;
      if (std::regex_search(acc, sm, startRe) &&
          (size_t)sm.position(0) < endPos) {
        bodyStart = (size_t)sm.position(0) + sm.length(0);
      }
      string body = acc.substr(bodyStart, endPos - bodyStart);
      if (bodyOut) {
        *bodyOut = body;
      } else {
        fwrite(body.data(), 1, body.size(), stdout);
      }
      return code;
    }
    ::usleep(80 * 1000);
  }
  fprintf(stderr, "etctl: run timed out after %.1fs\n", timeoutSec);
  return 124;
}

int cmdRun(const string& name, const string& command, double timeoutSec) {
  return runCommand(name, command, timeoutSec, nullptr);
}

int cmdOpen(int argc, char** argv) {
  /*
   * etctl open NAME [et-args...]  ->  et --ctl --name NAME [et-args...]
   *
   * NAME is a positional (consistent with the other verbs); etctl owns it and
   * translates it to et's --name.  The call is idempotent: if NAME is already a
   * live session, do nothing.  That makes `open` a cheap "ensure this session
   * exists" step you can safely run before driving it, which is the clean
   * version of etch's autospawn -- the connection details live only here, not on
   * every command.
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
  for (int i = 3; i < argc; i++) {
    string a = argv[i];
    if (a == "--ctl-socket" && i + 1 < argc) {
      checkTarget = argv[i + 1];
      i++;
    } else if (a.rfind("--ctl-socket=", 0) == 0) {
      checkTarget = a.substr(strlen("--ctl-socket="));
    } else if (a == "--name" && i + 1 < argc) {
      i++;  // skip et's --name value, it isn't the destination
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

  string etPath = "et";
  string self = argv[0];
  size_t slash = self.find_last_of('/');
  if (slash != string::npos) {
    string sibling = self.substr(0, slash + 1) + "et";
    if (::access(sibling.c_str(), X_OK) == 0) {
      etPath = sibling;  // prefer the et next to this etctl (dev/build trees)
    }
  }
  vector<char*> args;
  args.push_back(strdup(etPath.c_str()));
  args.push_back(strdup("--ctl"));
  args.push_back(strdup("--name"));
  args.push_back(strdup(name.c_str()));
  for (int i = 3; i < argc; i++) {
    args.push_back(strdup(argv[i]));
  }
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
        // verbs do; the `...` passthrough (not a real option) is appended in the
        // same column so the options block reads as one.
        cxxopts::Options opts("etctl open", descFor("open"));
        opts.add_options()("h,help", "Print help");
        opts.positional_help("");
        opts.custom_help("NAME [OPTION...] [user@]host[:port]");
        fputs(opts.help({""}).c_str(), stdout);
        printf(
            "  ...         Passed directly to et (see `et --help`)\n"
            "\n"
            "  NAME is the session name (etctl supplies et's --name);\n"
            "  if NAME is already running it does nothing.\n");
        return 0;
      }
    }
    return cmdOpen(argc, argv);
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
    string bytes = res.count("TEXT") ? res["TEXT"].as<string>() : readAllStdin();
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
    return cmdRun(name, res["CMD"].as<string>(), res["timeout"].as<double>());
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
