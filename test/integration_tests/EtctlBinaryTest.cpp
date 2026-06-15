// Drives the *real* etctl binary against an in-process ControlListener, so the
// shipped CLI's argv parsing and wire framing are exercised end-to-end.  Skipped
// (passes trivially) if the etctl binary isn't found next to the test, so it
// never breaks an unusual build layout.
#include <array>
#include <atomic>
#include <cstdio>

#include "ControlConsole.hpp"
#include "ControlListener.hpp"
#include "ControlPaths.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {

string etctlBinary() {
  if (const char* env = getenv("ETCTL_BIN")) {
    return string(env);
  }
  return "./etctl";  // ctest / manual runs start in the build dir
}

struct RunResult {
  string out;
  int code;
};

RunResult runEtctl(const string& args) {
  string cmd = etctlBinary() + " " + args + " 2>/dev/null";
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

}  // namespace

TEST_CASE("EtctlBinaryDrivesAControlSession", "[EtctlBinary]") {
  if (::access(etctlBinary().c_str(), X_OK) != 0) {
    WARN("etctl binary not found at " << etctlBinary() << "; skipping");
    SUCCEED();
    return;
  }

  const string name = "etctlbin_" + std::to_string(::getpid());
  control_paths::ensureControlDir();
  const string socketPath = control_paths::socketPathForName(name);

  auto console = std::make_shared<ControlConsole>();
  std::atomic<bool> done{false};
  ControlListener listener(console, socketPath, [&]() { done = true; });
  listener.start();

  // Echo injected input back into the scrollback, mimicking a terminal echo.
  std::thread echo([&]() {
    while (!done) {
      char buf[4096];
      ssize_t n = ::read(console->getFd(), buf, sizeof(buf));
      if (n > 0) {
        console->write(string(buf, n));
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
  });

  SECTION("info reports liveness and default size") {
    RunResult r = runEtctl("info " + name);
    REQUIRE(r.code == 0);
    REQUIRE(r.out.find("alive=1") != string::npos);
    REQUIRE(r.out.find("rows=24") != string::npos);
    REQUIRE(r.out.find("cols=132") != string::npos);
  }

  SECTION("writeln is injected, echoed, and read back") {
    REQUIRE(runEtctl("writeln " + name + " hello_etctl").code == 0);
    // Give the echo thread a beat to append it.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    RunResult r = runEtctl("read " + name);
    REQUIRE(r.code == 0);
    REQUIRE(r.out.find("hello_etctl") != string::npos);
  }

  SECTION("write injects a TEXT arg (raw, no newline)") {
    // write takes the text directly as an argument (not just stdin); it adds no
    // trailing newline, so the bytes land on the input line as typed.
    REQUIRE(runEtctl("write " + name + " write_arg_xyz").code == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    RunResult r = runEtctl("read " + name);
    REQUIRE(r.code == 0);
    REQUIRE(r.out.find("write_arg_xyz") != string::npos);
  }

  SECTION("sessions lists this session") {
    RunResult r = runEtctl("sessions");
    REQUIRE(r.out.find(name) != string::npos);
    REQUIRE(r.out.find("connected") != string::npos);
  }

  done = true;
  echo.join();
  listener.shutdown();
}
