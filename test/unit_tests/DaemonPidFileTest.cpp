#include "DaemonCreator.hpp"
#include "TestHeaders.hpp"

#ifndef WIN32
#include <fstream>

using namespace et;

namespace {
struct PidSandbox {
  string dir = test::makeTempDir("et_daemon_pid_security");
  string path = dir + "/daemon.pid";
  ~PidSandbox() { fs::remove_all(dir); }
};

void writeFixture(const string& path, const string& data) {
  std::ofstream out(path);
  out << data;
  out.close();
  if (!out) throw std::runtime_error("fixture write failed");
}

string readFixture(const string& path) {
  std::ifstream in(path);
  return string(std::istreambuf_iterator<char>(in), {});
}

// Bound special-file repros, including the old blocking open() on a FIFO.
int tryWritePid(const string& path) {
  const pid_t child = fork();
  if (child < 0) throw std::runtime_error("fork failed");
  if (child == 0) {
    try {
      DaemonCreator::writePidFile(path, 12345);
      _exit(0);
    } catch (...) {
      _exit(1);
    }
  }
  int status = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (waitpid(child, &status, WNOHANG) == 0) {
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(child, SIGKILL);
      waitpid(child, &status, 0);
      return 2;
    }
    testSleepMicros(10000);
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 3;
}
}  // namespace

TEST_CASE("Daemon pid writes refuse planted links without modifying victims",
          "[DaemonPidFile][security]") {
  PidSandbox sandbox;
  const string victim = sandbox.dir + "/victim";
  const string contents = "DO NOT OVERWRITE THIS FILE\n";
  writeFixture(victim, contents);
  REQUIRE(chmod(victim.c_str(), 0600) == 0);
  SECTION("Symlink") {
    REQUIRE(symlink(victim.c_str(), sandbox.path.c_str()) == 0);
  }
  SECTION("Hard link") {
    REQUIRE(link(victim.c_str(), sandbox.path.c_str()) == 0);
  }
  CHECK(tryWritePid(sandbox.path) == 1);
  CHECK(readFixture(victim) == contents);
}

TEST_CASE("Daemon pid writes reject special files without blocking",
          "[DaemonPidFile][security]") {
  PidSandbox sandbox;
  SECTION("FIFO") { REQUIRE(mkfifo(sandbox.path.c_str(), 0600) == 0); }
  SECTION("Directory") { REQUIRE(mkdir(sandbox.path.c_str(), 0700) == 0); }
  CHECK(tryWritePid(sandbox.path) == 1);
}

TEST_CASE("Daemon pid writes create private files and replace stale contents",
          "[DaemonPidFile][security]") {
  PidSandbox sandbox;
  DaemonCreator::writePidFile(sandbox.path, 123456789);
  DaemonCreator::writePidFile(sandbox.path, 42);
  CHECK(readFixture(sandbox.path) == "42\n");
  struct stat st;
  REQUIRE(lstat(sandbox.path.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
}

TEST_CASE("Daemon pid writes reject writable ancestors and files",
          "[DaemonPidFile][security]") {
  PidSandbox sandbox;
  const string contents = "UNCHANGED\n";
  writeFixture(sandbox.path, contents);
  REQUIRE(chmod(sandbox.path.c_str(), 0600) == 0);
  SECTION("Writable ancestor") {
    REQUIRE(chmod(sandbox.dir.c_str(), 0777) == 0);
  }
  SECTION("Writable file") { REQUIRE(chmod(sandbox.path.c_str(), 0666) == 0); }
  CHECK(tryWritePid(sandbox.path) == 1);
  CHECK(readFixture(sandbox.path) == contents);
}

TEST_CASE("Daemon pid writes reject files owned by another user even as root",
          "[DaemonPidFile][security][.root]") {
  if (geteuid() != 0) SKIP("Requires root to plant a different owner's file");
  PidSandbox sandbox;
  const string contents = "ATTACKER FILE\n";
  writeFixture(sandbox.path, contents);
  passwd* nobody = getpwnam("nobody");
  REQUIRE(nobody != nullptr);
  REQUIRE(nobody->pw_uid != 0);
  REQUIRE(chown(sandbox.path.c_str(), nobody->pw_uid, nobody->pw_gid) == 0);
  // Keep mode safe: rejection must be due to ownership, even though root
  // can otherwise overwrite a 0600 file owned by another user.
  REQUIRE(chmod(sandbox.path.c_str(), 0600) == 0);
  CHECK(tryWritePid(sandbox.path) == 1);
  CHECK(readFixture(sandbox.path) == contents);
}
#else
TEST_CASE("Daemon pid file Unix attack scenarios", "[DaemonPidFile]") {
  SKIP("Unix daemonization and ownership semantics");
}
#endif
