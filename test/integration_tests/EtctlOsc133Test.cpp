// End-to-end OSC-133 tests: drive the real etctl binary against a real shell
// running interactively behind a pty, with a minimal FinalTerm/iTerm2-style
// integration. Unlike EtctlRunTest (which wires a shell through plain pipes and
// therefore only exercises the echo-marker fallback), a pty lets the prompt
// hooks fire, so these verify the OSC-133 framings against a genuine
// integration:
//   - a zsh session (bracketed paste on by default, preexec/precmd emit C/D)
//     exercises kBracketOsc133 -- the bare command is pasted, so the real
//     command is what runs, with no eval-wrapper and no injected markers;
//   - a bash session with OSC-133 but no bracketed paste exercises kEvalOsc133
//     (eval here-doc + OSC read) and, forced, the kMark echo-marker fallback;
//   - a fish session (bracketed paste on, but no OSC-133 and no eval here-doc)
//     exercises kBracketMark -- the bare command is pasted and boundaries come
//     from echo-markers, with exit codes read from fish's $status.
// Skipped when the shell or etctl is unavailable.
#include <atomic>
#include <fstream>
#include <functional>
#include <thread>

#if __APPLE__
#include <util.h>
#elif __FreeBSD__
#include <libutil.h>
#else
#include <pty.h>
#endif
#include <fcntl.h>
#include <sys/wait.h>
#include <termios.h>

#include "ControlConsole.hpp"
#include "ControlListener.hpp"
#include "ControlPaths.hpp"
#include "RawSocketUtils.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {

string etctlBin() {
  if (const char* env = getenv("ETCTL_BIN")) return string(env);
  return "./etctl";
}

string firstExecutable(std::initializer_list<const char*> paths) {
  for (const char* p : paths)
    if (::access(p, X_OK) == 0) return string(p);
  return "";
}
string zshPath() {
  return firstExecutable({"/bin/zsh", "/usr/bin/zsh", "/opt/homebrew/bin/zsh"});
}
string fishPath() {
  return firstExecutable({"/opt/homebrew/bin/fish", "/usr/local/bin/fish",
                          "/usr/bin/fish", "/bin/fish"});
}
struct RunResult {
  string out;
  int code;
};

RunResult runEtctl(const string& args) {
  const string cmd = etctlBin() + " " + args + " 2>&1";
  RunResult r;
  FILE* p = popen(cmd.c_str(), "r");
  REQUIRE(p != nullptr);
  std::array<char, 4096> buf;
  size_t n;
  while ((n = fread(buf.data(), 1, buf.size(), p)) > 0)
    r.out.append(buf.data(), n);
  int status = pclose(p);
  r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return r;
}

// Fork `childExec` behind a pty, wire the pty to a ControlConsole +
// ControlListener (so the real etctl binary can drive it by name over the
// control socket), let the shell reach its first prompt, then run `body` with
// the session name and tear everything down.
void drivePty(const std::function<void()>& childExec,
              const std::function<void(const string& name)>& body) {
  int master = -1;
  pid_t pid = forkpty(&master, nullptr, nullptr, nullptr);
  REQUIRE(pid >= 0);
  if (pid == 0) {
    childExec();
    _exit(127);
  }
  int flags = fcntl(master, F_GETFL, 0);
  fcntl(master, F_SETFL, flags | O_NONBLOCK);

  const string name = "etctlosc_" + std::to_string(::getpid());
  control_paths::ensureControlDir();
  const string socketPath = control_paths::socketPathForName(name);

  auto console = std::make_shared<ControlConsole>();
  std::atomic<bool> done{false};
  ControlListener listener(console, socketPath, [&]() { done = true; });
  listener.start();

  std::thread inRelay([&]() {
    try {
      while (!done) {
        char buf[4096];
        ssize_t n = ::read(console->getFd(), buf, sizeof(buf));
        if (n > 0)
          RawSocketUtils::writeAll(master, buf, n);
        else
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } catch (const std::exception&) {
    }
  });
  std::thread outRelay([&]() {
    try {
      while (!done) {
        char buf[4096];
        ssize_t n = ::read(master, buf, sizeof(buf));
        if (n > 0)
          console->write(string(buf, n));
        else
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } catch (const std::exception&) {
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  body(name);

  done = true;
  inRelay.join();
  outRelay.join();
  listener.shutdown();
  ::close(master);
  ::kill(pid, SIGTERM);
  int st;
  ::waitpid(pid, &st, 0);
}

// Minimal zsh OSC-133 integration: preexec emits C, precmd emits D;$?. zsh has
// bracketed paste on by default, so etctl detects kBracketOsc133 and injects
// the bare command.
const char* kZshRc =
    "PROMPT='$ '\n"
    "preexec() { printf '\\033]133;C\\007' }\n"
    "precmd() { printf '\\033]133;D;%s\\007' $? }\n";

// Minimal zsh OSC-133 integration with bracketed paste turned OFF, so etctl
// detects kEvalOsc133 (eval here-doc + OSC read) rather than kBracketOsc133.
const char* kZshEvalRc =
    "PROMPT='$ '\n"
    "unset zle_bracketed_paste\n"
    "preexec() { printf '\\033]133;C\\007' }\n"
    "precmd() { printf '\\033]133;D;%s\\007' $? }\n";

// A throwaway ZDOTDIR holding a .zshrc, so the real ~/.zshrc doesn't interfere.
string makeZdot(const char* rc) {
  char dirTmpl[] = "/tmp/etctl_zdot_XXXXXX";
  const string dir = ::mkdtemp(dirTmpl);
  const string zrc = dir + "/.zshrc";
  int fd = ::open(zrc.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  REQUIRE(fd >= 0);
  RawSocketUtils::writeAll(fd, rc, strlen(rc));
  ::close(fd);
  return dir;
}

// Drive `body` against a zsh session whose ZDOTDIR/.zshrc is `rc`, behind a
// pty.
void driveZsh(const char* rc,
              const std::function<void(const string& name)>& body) {
  const string zdot = makeZdot(rc);
  const string zsh = zshPath();
  drivePty(
      [&]() {
        setenv("TERM", "xterm", 1);
        setenv("HOME", zdot.c_str(), 1);
        setenv("ZDOTDIR", zdot.c_str(), 1);
        execl(zsh.c_str(), "zsh", "-i", (char*)nullptr);
      },
      body);
}

// Drive `body` against an interactive fish behind a pty. Default fish has
// bracketed paste on but emits no OSC 133 and cannot parse the eval here-doc, so
// etctl detects kBracketMark. An isolated HOME keeps the user's fish config out,
// and an empty greeting keeps the first prompt clean.
void driveFish(const std::function<void(const string& name)>& body) {
  char dirTmpl[] = "/tmp/etctl_fish_XXXXXX";
  const string home = ::mkdtemp(dirTmpl);
  const string cfgDir = home + "/.config/fish";
  { const string cmd = "mkdir -p '" + cfgDir + "'"; (void)::system(cmd.c_str()); }
  const string cfg = cfgDir + "/config.fish";
  int fd = ::open(cfg.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  REQUIRE(fd >= 0);
  const char* rc = "set -g fish_greeting ''\n";
  RawSocketUtils::writeAll(fd, rc, strlen(rc));
  ::close(fd);
  const string fish = fishPath();
  drivePty(
      [&]() {
        setenv("TERM", "xterm-256color", 1);
        setenv("HOME", home.c_str(), 1);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_DATA_HOME");
        execl(fish.c_str(), "fish", "-i", (char*)nullptr);
      },
      body);
}

}  // namespace

TEST_CASE("EtctlRunBracketedOsc133 (zsh)", "[EtctlOsc133]") {
  if (::access(etctlBin().c_str(), X_OK) != 0 || zshPath().empty()) {
    WARN("etctl or zsh unavailable; skipping bracketed OSC-133 pty test");
    SUCCEED();
    return;
  }
  driveZsh(kZshRc, [&](const string& name) {
    {
      // Auto-detect -> kBracketOsc133. The bare command runs, so the real
      // command is in the scrollback (no eval-wrapper) with no markers.
      RunResult r = runEtctl("run " + name + " 'echo hi_zsh' --timeout 10");
      INFO("auto -> code=" << r.code << " out=[" << r.out << "]");
      CHECK(r.code == 0);
      CHECK(r.out.find("hi_zsh") != string::npos);
      CHECK(r.out.find("ETCTL_") == string::npos);
    }
    {
      // Multi-line body runs as one command via the paste.
      RunResult r = runEtctl("run " + name +
                             " 'X=42\nprintf \"v=[%s]\" \"$X\"' --timeout 10");
      INFO("multiline -> code=" << r.code << " out=[" << r.out << "]");
      CHECK(r.code == 0);
      CHECK(r.out == "v=[42]");
    }
    {
      RunResult r = runEtctl("run " + name + " '(exit 7)' --timeout 10");
      CHECK(r.code == 7);
      CHECK(r.out.find("ETCTL_") == string::npos);
    }
    {
      // No trailing newline: body is exactly the output.
      RunResult r = runEtctl("run " + name + " 'printf foo' --timeout 10");
      CHECK(r.code == 0);
      CHECK(r.out == "foo");
    }
    {
      // History expansion is disabled on detect, so a bare `!` is literal.
      RunResult r = runEtctl("run " + name + " 'echo a!b' --timeout 10");
      CHECK(r.code == 0);
      CHECK(r.out.find("a!b") != string::npos);
    }
    {
      // Malformed body parks on a continuation prompt; run must abort (not
      // hang) and recover the session.
      RunResult r = runEtctl("run " + name + " 'echo \"oops' --timeout 30");
      INFO("malformed -> code=" << r.code << " out=[" << r.out << "]");
      CHECK(r.code == 125);
    }
    {
      // Session still healthy after the abort.
      RunResult r = runEtctl("run " + name + " 'echo alive' --timeout 10");
      CHECK(r.code == 0);
      CHECK(r.out.find("alive") != string::npos);
    }
  });
}

TEST_CASE("EtctlRunEvalOsc133AndMarkers (zsh, no bracketed paste)",
          "[EtctlOsc133]") {
  if (::access(etctlBin().c_str(), X_OK) != 0 || zshPath().empty()) {
    WARN("etctl or zsh unavailable; skipping eval/marker OSC-133 pty test");
    SUCCEED();
    return;
  }
  driveZsh(kZshEvalRc, [&](const string& name) {
    {
      // Auto-detect -> kEvalOsc133 (OSC 133 but no bracketed paste): eval
      // here-doc injection, boundaries/exit from OSC 133, no echo markers.
      RunResult r = runEtctl("run " + name + " 'echo hi_eval' --timeout 10");
      INFO("eval-osc -> code=" << r.code << " out=[" << r.out << "]");
      CHECK(r.code == 0);
      CHECK(r.out.find("hi_eval") != string::npos);
      CHECK(r.out.find("ETCTL_") == string::npos);
    }
    {
      // Multi-line via the eval here-doc runs as one command.
      RunResult r = runEtctl("run " + name +
                             " 'Y=9\nprintf \"e=[%s]\" \"$Y\"' --timeout 10");
      CHECK(r.code == 0);
      CHECK(r.out == "e=[9]");
    }
    {
      RunResult r = runEtctl("run " + name + " '(exit 137)' --timeout 10");
      CHECK(r.code == 137);
    }
    {
      RunResult r = runEtctl("run " + name + " 'printf tight' --timeout 10");
      CHECK(r.code == 0);
      CHECK(r.out == "tight");
    }
    {
      // Forced echo-marker fallback: still correct, works on any shell.
      RunResult r = runEtctl("run --framing=mark " + name +
                             " 'echo hi_mark' --timeout 10");
      INFO("mark -> code=" << r.code << " out=[" << r.out << "]");
      CHECK(r.code == 0);
      CHECK(r.out.find("hi_mark") != string::npos);
    }
  });
}

TEST_CASE("EtctlRunBracketMark (fish)", "[EtctlOsc133]") {
  if (::access(etctlBin().c_str(), X_OK) != 0 || fishPath().empty()) {
    WARN("etctl or fish unavailable; skipping kBracketMark pty test");
    SUCCEED();
    return;
  }
  driveFish([&](const string& name) {
    // Force fresh detection: a single-process run can leave a stale cache under
    // the pid-based name from an earlier (zsh) case.
    const string cachePath = control_paths::controlDir() + "/" + name + ".framing";
    ::unlink(cachePath.c_str());
    // Probe detection once. fish 4.x blocks its interactive startup on a full
    // terminal-query handshake (XTVERSION/color/XTGETTCAP/DA1) that a headless
    // control session cannot fully satisfy, so it is not drivable here; detect
    // that and skip rather than fail. fish 3.x drives fine and runs the battery.
    runEtctl("run " + name + " 'true' --timeout 6");
    {
      std::ifstream cf(cachePath);
      string cached;
      std::getline(cf, cached);
      if (cached != "1 0 1") {
        WARN("fish not headless-drivable (detected framing '"
             << cached << "', e.g. fish 4.x query handshake); skipping battery");
        SUCCEED();
        return;
      }
    }
    {
      // Auto-detect -> kBracketMark: fish has bracketed paste but no OSC 133 and
      // cannot parse the eval here-doc. The bare command is pasted; output comes
      // from echo-markers, which must not leak into the body.
      RunResult r = runEtctl("run " + name + " 'echo hi_fish' --timeout 10");
      INFO("auto -> code=" << r.code << " out=[" << r.out << "]");
      CHECK(r.code == 0);
      CHECK(r.out.find("hi_fish") != string::npos);
      CHECK(r.out.find("ETCTL_") == string::npos);
    }
    {
      // Detection recorded fish: bracket 1, oscRead 0, statusVar 1
      // (kBracketMark, $status).
      std::ifstream f(control_paths::controlDir() + "/" + name + ".framing");
      string cached;
      std::getline(f, cached);
      INFO("cache=[" << cached << "]");
      CHECK(cached == "1 0 1");
    }
    {
      // Multi-line body (fish syntax) runs as one command via the paste.
      RunResult r =
          runEtctl("run " + name +
                   " 'set X 42\nprintf \"v=[%s]\" \"$X\"' --timeout 10");
      INFO("multiline -> code=" << r.code << " out=[" << r.out << "]");
      CHECK(r.code == 0);
      CHECK(r.out == "v=[42]");
    }
    {
      // Exit code flows through fish's $status (not $?).
      RunResult r = runEtctl("run " + name + " 'sh -c \"exit 7\"' --timeout 10");
      CHECK(r.code == 7);
      CHECK(r.out.find("ETCTL_") == string::npos);
    }
    {
      // No trailing newline: body is exactly the output.
      RunResult r = runEtctl("run " + name + " 'printf foo' --timeout 10");
      CHECK(r.code == 0);
      CHECK(r.out == "foo");
    }
    {
      // fish has no ! history expansion (and we skip no_bang_hist for it), so a
      // bare ! is literal.
      RunResult r = runEtctl("run " + name + " 'echo a!b' --timeout 10");
      CHECK(r.code == 0);
      CHECK(r.out.find("a!b") != string::npos);
    }
    {
      // Malformed body parks on a continuation prompt; run must abort. There is
      // no OSC 133 C mark, so the guard keys on the executed start marker.
      RunResult r = runEtctl("run " + name + " 'echo \"oops' --timeout 30");
      INFO("malformed -> code=" << r.code << " out=[" << r.out << "]");
      CHECK(r.code == 125);
    }
    {
      // Session still healthy after the abort.
      RunResult r = runEtctl("run " + name + " 'echo alive' --timeout 10");
      CHECK(r.code == 0);
      CHECK(r.out.find("alive") != string::npos);
    }
  });
}
