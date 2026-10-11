#include "TelemetryService.hpp"
#include "TestHeaders.hpp"

#ifndef WIN32
#include <fstream>

using namespace et;

namespace {
class TelemetrySandbox {
 public:
  string dir = test::makeTempDir("et_telemetry_security");
  string config;
  string database = dir + "/database";

  TelemetrySandbox() {
    for (const char* name :
         {"HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "ET_NO_TELEMETRY"}) {
      const char* value = getenv(name);
      saved[name] = value ? optional<string>(value) : nullopt;
    }
    setenv("HOME", dir.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (dir + "/config").c_str(), 1);
    setenv("XDG_CACHE_HOME", (dir + "/cache").c_str(), 1);
    unsetenv("ET_NO_TELEMETRY");
    config = sago::getConfigHome() + "/et";
    fs::create_directories(fs::path(config).parent_path());
  }

  ~TelemetrySandbox() {
    for (const auto& item : saved) {
      if (item.second) {
        setenv(item.first.c_str(), item.second->c_str(), 1);
      } else {
        unsetenv(item.first.c_str());
      }
    }
    fs::remove_all(dir);
  }

  // Run real telemetry initialization in a child: Sentry's global state and
  // fatal errors cannot contaminate the runner. Disable uploads at runtime,
  // even when the application's SDK has a real network transport compiled in.
  int initialize() {
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) {
      umask(0022);
      class InspectTelemetry : public TelemetryService {
       public:
        explicit InspectTelemetry(const string& path)
            : TelemetryService(true, path, "security-test", false) {}
        bool enabled() const { return allowed; }
      };
      try {
        InspectTelemetry telemetry(database);
        const bool enabled = telemetry.enabled();
        telemetry.shutdown();
        _exit(enabled ? 0 : 1);
      } catch (...) {
        _exit(2);
      }
    }
    int status = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (waitpid(child, &status, WNOHANG) == 0) {
      if (std::chrono::steady_clock::now() >= deadline) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        return 3;
      }
      testSleepMicros(10000);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 4;
  }

 private:
  map<string, optional<string>> saved;
};

mode_t modeOf(const string& path) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0) throw std::runtime_error("lstat failed");
  return st.st_mode & 0777;
}

const string configContents =
    "[Sentry]\nId = 11111111-1111-4111-8111-111111111111\n";

void writeFixture(const string& path, const string& contents) {
  std::ofstream out(path);
  out << contents;
  out.close();
  if (!out) throw std::runtime_error("could not write fixture");
}

string readFixture(const string& path) {
  std::ifstream in(path);
  return string(std::istreambuf_iterator<char>(in), {});
}
}  // namespace

TEST_CASE("Telemetry creates private configuration and preserves its id",
          "[TelemetryStorage][security]") {
#ifdef NO_TELEMETRY
  SKIP("Telemetry compiled out");
#else
  TelemetrySandbox sandbox;
  REQUIRE(sandbox.initialize() == 0);
  CHECK(modeOf(sandbox.config) == 0700);
  CHECK(modeOf(sandbox.config + "/telemetry.ini") == 0600);
  const string contents = readFixture(sandbox.config + "/telemetry.ini");
  REQUIRE(sandbox.initialize() == 0);
  CHECK(readFixture(sandbox.config + "/telemetry.ini") == contents);
#endif
}

TEST_CASE("Telemetry refuses planted config links and special files",
          "[TelemetryStorage][security]") {
#ifdef NO_TELEMETRY
  SKIP("Telemetry compiled out");
#else
  TelemetrySandbox sandbox;
  REQUIRE(mkdir(sandbox.config.c_str(), 0700) == 0);
  const string victim = sandbox.dir + "/victim";
  writeFixture(victim, configContents);
  REQUIRE(chmod(victim.c_str(), 0600) == 0);
  const string path = sandbox.config + "/telemetry.ini";
  SECTION("Symlink") { REQUIRE(symlink(victim.c_str(), path.c_str()) == 0); }
  SECTION("Hard link") { REQUIRE(link(victim.c_str(), path.c_str()) == 0); }
  SECTION("FIFO must be rejected without blocking") {
    REQUIRE(mkfifo(path.c_str(), 0600) == 0);
  }
  CHECK(sandbox.initialize() == 1);
  CHECK(readFixture(victim) == configContents);
#endif
}

TEST_CASE("Telemetry refuses a symlinked configuration directory",
          "[TelemetryStorage][security]") {
#ifdef NO_TELEMETRY
  SKIP("Telemetry compiled out");
#else
  TelemetrySandbox sandbox;
  const string victim = sandbox.dir + "/victim";
  REQUIRE(mkdir(victim.c_str(), 0700) == 0);
  REQUIRE(symlink(victim.c_str(), sandbox.config.c_str()) == 0);
  CHECK(sandbox.initialize() == 1);
  CHECK(fs::is_empty(victim));
#endif
}

TEST_CASE("Telemetry refuses a writable configuration directory",
          "[TelemetryStorage][security]") {
#ifdef NO_TELEMETRY
  SKIP("Telemetry compiled out");
#else
  TelemetrySandbox sandbox;
  REQUIRE(mkdir(sandbox.config.c_str(), 0700) == 0);
  REQUIRE(chmod(sandbox.config.c_str(), 0777) == 0);
  CHECK(sandbox.initialize() == 1);
  CHECK(fs::is_empty(sandbox.config));
#endif
}

TEST_CASE("Telemetry refuses configuration under an unsafe ancestor",
          "[TelemetryStorage][security]") {
#ifdef NO_TELEMETRY
  SKIP("Telemetry compiled out");
#else
  TelemetrySandbox sandbox;
  REQUIRE(chmod(fs::path(sandbox.config).parent_path().c_str(), 0777) == 0);
  CHECK(sandbox.initialize() == 1);
  CHECK(!fs::exists(sandbox.config));
#endif
}

TEST_CASE(
    "Telemetry bounds existing configuration reads and migrates permissions",
    "[TelemetryStorage][security]") {
#ifdef NO_TELEMETRY
  SKIP("Telemetry compiled out");
#else
  TelemetrySandbox sandbox;
  REQUIRE(mkdir(sandbox.config.c_str(), 0755) == 0);
  const string path = sandbox.config + "/telemetry.ini";
  SECTION("Existing identity survives permissions migration") {
    writeFixture(path, configContents);
    REQUIRE(chmod(path.c_str(), 0644) == 0);
    REQUIRE(sandbox.initialize() == 0);
    CHECK(readFixture(path) == configContents);
    CHECK(modeOf(sandbox.config) == 0700);
    CHECK(modeOf(path) == 0600);
  }
  SECTION("Oversized configuration is refused") {
    const string oversized(64 * 1024 + 1, 'a');
    writeFixture(path, oversized);
    REQUIRE(chmod(path.c_str(), 0600) == 0);
    CHECK(sandbox.initialize() == 1);
    CHECK(readFixture(path) == oversized);
  }
#endif
}

TEST_CASE("Sentry refuses a planted database directory before writing to it",
          "[TelemetryStorage][security][CVE-2023-23558]") {
#if !defined(USE_SENTRY) || defined(NO_TELEMETRY)
  SKIP("Requires a build with Sentry and telemetry enabled");
#else
  TelemetrySandbox sandbox;
  REQUIRE(mkdir(sandbox.database.c_str(), 0700) == 0);
  REQUIRE(chmod(sandbox.database.c_str(), 0777) == 0);
  CHECK(sandbox.initialize() == 1);
  CHECK(fs::is_empty(sandbox.database));
#endif
}

TEST_CASE("Sentry refuses another user's database even when running as root",
          "[TelemetryStorage][security][.root]") {
#if !defined(USE_SENTRY) || defined(NO_TELEMETRY)
  SKIP("Requires a build with Sentry and telemetry enabled");
#else
  if (geteuid() != 0) SKIP("Optional root ownership repro");
  TelemetrySandbox sandbox;
  REQUIRE(mkdir(sandbox.database.c_str(), 0700) == 0);
  passwd* nobody = getpwnam("nobody");
  REQUIRE(nobody != nullptr);
  REQUIRE(nobody->pw_uid != 0);
  REQUIRE(chown(sandbox.database.c_str(), nobody->pw_uid, nobody->pw_gid) == 0);
  // Isolate the ownership check from the writable-directory check above.
  REQUIRE(chmod(sandbox.database.c_str(), 0700) == 0);
  CHECK(sandbox.initialize() == 1);
  CHECK(fs::is_empty(sandbox.database));
#endif
}

TEST_CASE("Sentry defaults to a private cache outside shared temporary storage",
          "[TelemetryStorage][security][CVE-2023-23558]") {
#if !defined(USE_SENTRY) || defined(NO_TELEMETRY)
  SKIP("Requires a build with Sentry and telemetry enabled");
#else
  TelemetrySandbox sandbox;
  sandbox.database.clear();
  REQUIRE(sandbox.initialize() == 0);
  const string cache = sago::getCacheDir() + "/et";
  CHECK(modeOf(cache) == 0700);
  CHECK(modeOf(cache + "/sentry-client") == 0700);
#endif
}

TEST_CASE("Sentry checks ancestors of trusted system symlink targets",
          "[TelemetryStorage][security][.root]") {
#if !defined(USE_SENTRY) || defined(NO_TELEMETRY)
  SKIP("Requires a build with Sentry and telemetry enabled");
#else
  if (geteuid() != 0) SKIP("Requires root to create a trusted-owner alias");
  TelemetrySandbox sandbox;
  const string unsafe = sandbox.dir + "/unsafe";
  const string victim = unsafe + "/victim";
  const string alias = sandbox.dir + "/alias";
  REQUIRE(mkdir(unsafe.c_str(), 0700) == 0);
  REQUIRE(mkdir(victim.c_str(), 0700) == 0);
  REQUIRE(chmod(unsafe.c_str(), 0777) == 0);
  REQUIRE(symlink(victim.c_str(), alias.c_str()) == 0);
  sandbox.database = alias + "/database";
  CHECK(sandbox.initialize() == 1);
  CHECK(fs::is_empty(victim));
#endif
}
#else
TEST_CASE("Telemetry storage Unix attack scenarios", "[TelemetryStorage]") {
  SKIP("Unix file ownership and symlink semantics");
}
#endif
