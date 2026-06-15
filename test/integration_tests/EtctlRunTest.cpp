// Drives the real etctl binary against a real bash wired behind a ControlConsole
// + ControlListener via pipes.  bash stands in for the remote shell, so this
// verifies the full interactive data plane end-to-end: run() clean output + exit
// codes, writeln, read, and expect, all against a program that actually
// executes commands.  Skipped if etctl or bash is unavailable.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>

#include "ControlConsole.hpp"
#include "ControlListener.hpp"
#include "ControlPaths.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {

string etctlBin() {
  if (const char* env = getenv("ETCTL_BIN")) return string(env);
  return "./etctl";
}

string bashPath() {
  if (::access("/bin/bash", X_OK) == 0) return "/bin/bash";
  if (::access("/usr/bin/bash", X_OK) == 0) return "/usr/bin/bash";
  return "";
}

struct RunResult {
  string out;
  int code;
};

RunResult runEtctl(const string& args) {
  string cmd = etctlBin() + " " + args + " 2>&1";
  RunResult r;
  FILE* p = popen(cmd.c_str(), "r");
  REQUIRE(p != nullptr);
  std::array<char, 4096> buf;
  size_t n;
  while ((n = fread(buf.data(), 1, buf.size(), p)) > 0) {
    r.out.append(buf.data(), n);
  }
  int status = pclose(p);
  r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return r;
}

void setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

TEST_CASE("EtctlRunAgainstRealShell", "[EtctlRun]") {
  if (::access(etctlBin().c_str(), X_OK) != 0 || bashPath().empty()) {
    WARN("etctl or bash unavailable; skipping live-shell run test");
    SUCCEED();
    return;
  }

  // Fork bash wired to two pipes BEFORE we spawn any threads.
  int toShell[2];    // parent writes -> bash stdin
  int fromShell[2];  // bash stdout/stderr -> parent reads
  REQUIRE(::pipe(toShell) == 0);
  REQUIRE(::pipe(fromShell) == 0);

  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::dup2(toShell[0], STDIN_FILENO);
    ::dup2(fromShell[1], STDOUT_FILENO);
    ::dup2(fromShell[1], STDERR_FILENO);
    ::close(toShell[0]);
    ::close(toShell[1]);
    ::close(fromShell[0]);
    ::close(fromShell[1]);
    execl(bashPath().c_str(), "bash", "--norc", "--noprofile", (char*)NULL);
    _exit(127);
  }
  ::close(toShell[0]);
  ::close(fromShell[1]);
  setNonBlocking(fromShell[0]);

  const string name = "etctlrun_" + std::to_string(::getpid());
  control_paths::ensureControlDir();
  const string socketPath = control_paths::socketPathForName(name);

  auto console = std::make_shared<ControlConsole>();
  std::atomic<bool> done{false};
  ControlListener listener(console, socketPath, [&]() { done = true; });
  listener.start();

  // Relay A: injected input (etctl write) -> bash stdin.
  std::thread inRelay([&]() {
    try {
      while (!done) {
        char buf[4096];
        ssize_t n = ::read(console->getFd(), buf, sizeof(buf));
        if (n > 0) {
          RawSocketUtils::writeAll(toShell[1], buf, n);
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
    } catch (const std::exception&) {
      // bash went away; end the relay quietly.
    }
  });
  // Relay B: bash output -> scrollback (what etctl read sees).
  std::thread outRelay([&]() {
    try {
      while (!done) {
        char buf[4096];
        ssize_t n = ::read(fromShell[0], buf, sizeof(buf));
        if (n > 0) {
          console->write(string(buf, n));
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
    } catch (const std::exception&) {
    }
  });

  // Single flow with non-throwing CHECK so teardown (thread joins) is always
  // reached even on failure; a stray std::thread would otherwise terminate().
  {
    RunResult r = runEtctl("run " + name + " 'echo hello_shell' --timeout 10");
    INFO("run echo -> code=" << r.code << " out=[" << r.out << "]");
    CHECK(r.code == 0);
    CHECK(r.out.find("hello_shell") != string::npos);
    CHECK(r.out.find("ETCTL_") == string::npos);  // no marker leakage
  }
  {
    // Multi-line command: the body runs as one unit (eval of a here-doc), so a var
    // set on the first line is still visible on the last (no early line submission).
    RunResult r = runEtctl("run " + name +
                           " 'a=2\necho row1\necho row2\necho n=$a' --timeout 10");
    INFO("run multiline -> code=" << r.code << " out=[" << r.out << "]");
    CHECK(r.code == 0);
    CHECK(r.out.find("row1") != string::npos);
    CHECK(r.out.find("row2") != string::npos);
    CHECK(r.out.find("n=2") != string::npos);
  }
  {
    RunResult r = runEtctl("run " + name + " '(exit 7)' --timeout 10");
    INFO("run exit7 -> code=" << r.code << " out=[" << r.out << "]");
    CHECK(r.code == 7);
  }
  {
    // Multi-digit exit code: guards against the end-marker regex matching a
    // truncated code (the marker must be parsed as the whole "<n>\n", not a
    // greedy prefix).
    RunResult r = runEtctl("run " + name + " '(exit 137)' --timeout 10");
    INFO("run exit137 -> code=" << r.code << " out=[" << r.out << "]");
    CHECK(r.code == 137);
  }
  {
    RunResult r = runEtctl("run " + name + " false --timeout 10");
    INFO("run false -> code=" << r.code << " out=[" << r.out << "]");
    CHECK(r.code == 1);
  }
  {
    // Frame robustness: a body that fails to *parse* must not desync the frame.
    // The body is here-doc data fed to eval, so the parse error surfaces at eval
    // runtime; the end marker still prints and run returns a real, non-timeout
    // code.  The old brace-group wrap let a body parse error swallow the trailing
    // '}; echo <mark>:$?', so run hung until timeout (124).
    RunResult r = runEtctl("run " + name + " 'echo before; for' --timeout 10");
    INFO("run parse-error -> code=" << r.code << " out=[" << r.out << "]");
    CHECK(r.code != 0);    // the syntax error surfaces
    CHECK(r.code != 124);  // but did NOT time out / desync the frame
    CHECK(r.out.find("ETCTL_") == string::npos);  // no marker leakage
  }
  {
    // Frame robustness: brace / bang / quote metacharacters in the body are inert,
    // since it is quoted here-doc data, not source the line reader parses.  (A bare
    // '}' or '!' in the body used to be able to break the brace-group wrap.)
    RunResult r = runEtctl("run " + name + " 'echo \"a!b{c}d\"' --timeout 10");
    INFO("run metachars -> code=" << r.code << " out=[" << r.out << "]");
    CHECK(r.code == 0);
    CHECK(r.out.find("a!b{c}d") != string::npos);
  }
  {
    RunResult w = runEtctl("writeln " + name + " 'echo via_writeline'");
    INFO("writeln -> code=" << w.code << " out=[" << w.out << "]");
    CHECK(w.code == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    RunResult r = runEtctl("read " + name + " --strip");
    INFO("read -> out=[" << r.out << "]");
    CHECK(r.out.find("via_writeline") != string::npos);
  }
  {
    CHECK(runEtctl("writeln " + name + " 'echo MARKER_zzz'").code == 0);
    RunResult r = runEtctl("expect " + name + " MARKER_zzz --timeout 10 --from-start");
    INFO("expect -> code=" << r.code << " out=[" << r.out << "]");
    CHECK(r.code == 0);
  }

  done = true;
  inRelay.join();
  outRelay.join();
  listener.shutdown();
  ::close(toShell[1]);
  ::close(fromShell[0]);
  ::kill(pid, SIGTERM);
  int st;
  ::waitpid(pid, &st, 0);
}
