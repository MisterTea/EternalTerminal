#include <ftw.h>

#include <optional>

#include "SessionStore.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {

int RemoveDirectory(const char* path) {
  // Use posix file tree walk to traverse the directory and remove the contents.
  return nftw(
      path,
      [](const char* fpath, const struct stat* sb, int typeflag,
         struct FTW* ftwbuf) { return ::remove(fpath); },
      64,  // Maximum open fds.
      FTW_DEPTH | FTW_PHYS);
}

class TestEnvironment {
 public:
  string createTempDir() {
    string tmpPath = GetTempDirectory() + string("et_session_XXXXXXXX");
    const string dir = string(mkdtemp(&tmpPath[0]));

    temporaryDirs.push_back(dir);
    return dir;
  }

  mode_t fileMode(const string& path) {
    struct stat fileStat;
    if (::stat(path.c_str(), &fileStat) != 0) {
      return 0;
    }
    return fileStat.st_mode & 0777;
  }

  // Codespaces and similar environments may enforce additional ACLs, so verify
  // that the permissions are less than a certain maximum. See
  // https://github.community/t/bug-umask-does-not-seem-to-be-respected/129638
  void requireModeLessPrivilegedThan(const string& path, mode_t highestMode) {
    const mode_t mode = fileMode(path);
    INFO("path=" << path << " mode=" << oct << mode);
    REQUIRE((mode & highestMode) == mode);
  }

  string setHomeDir(const string& newHome) {
    backupEnv("HOME");
    ::setenv("HOME", newHome.c_str(), 1);
    return newHome;
  }

  ~TestEnvironment() {
    for (const string& dir : temporaryDirs) {
      const int removeResult = RemoveDirectory(dir.c_str());
      if (removeResult == -1) {
        LOG(ERROR) << "Error when removing dir: " << dir;
        FATAL_FAIL(removeResult);
      }
    }

    for (const auto& [key, value] : savedEnvs) {
      if (value) {
        ::setenv(key.c_str(), value->c_str(), 1);
      } else {
        ::unsetenv(key.c_str());
      }
    }
  }

 private:
  void backupEnv(const char* name) {
    if (savedEnvs.count(name)) {
      return;
    }
    const char* previousValue = ::getenv(name);
    if (previousValue) {
      savedEnvs[string(name)] = string(previousValue);
    } else {
      savedEnvs[string(name)] = std::nullopt;
    }
  }

  vector<string> temporaryDirs;
  map<string, optional<string>> savedEnvs;
};

SessionInfo makeInfo(const string& name, const string& host = "nas",
                     int port = 2022, const string& id = "client-id",
                     const string& passkey = string(32, 'k')) {
  SessionInfo info;
  info.name = name;
  info.host = host;
  info.port = port;
  info.id = id;
  info.passkey = passkey;
  info.savedAt = 1755645600;
  return info;
}

}  // namespace

TEST_CASE("SessionStore name validation", "[SessionStore]") {
  REQUIRE(isValidSessionName("alpha"));
  REQUIRE(isValidSessionName("a"));
  REQUIRE(isValidSessionName("my-session_1.2"));
  REQUIRE(isValidSessionName("nas-20260820-060421"));
  // Max length is 63 characters total.
  REQUIRE(isValidSessionName(string(63, 'a')));
  REQUIRE_FALSE(isValidSessionName(""));
  REQUIRE_FALSE(isValidSessionName(string(64, 'a')));
  REQUIRE_FALSE(isValidSessionName("-leading-dash"));
  REQUIRE_FALSE(isValidSessionName("./relative"));
  REQUIRE_FALSE(isValidSessionName("../escape"));
  REQUIRE_FALSE(isValidSessionName("with/slash"));
  REQUIRE_FALSE(isValidSessionName("with space"));
  REQUIRE_FALSE(isValidSessionName("colon:name"));
  REQUIRE_FALSE(isValidSessionName("\nnewline"));
}

TEST_CASE("SessionStore save/load round trip", "[SessionStore]") {
  TestEnvironment env;
  const string home = env.setHomeDir(env.createTempDir());

  const SessionInfo info =
      makeInfo("alpha", "10.0.0.5", 9922, "id-abc", string(32, 'p'));
  saveSession(info);

  const optional<SessionInfo> loaded = loadSession("alpha");
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->name == "alpha");
  REQUIRE(loaded->host == "10.0.0.5");
  REQUIRE(loaded->port == 9922);
  REQUIRE(loaded->id == "id-abc");
  REQUIRE(loaded->passkey == string(32, 'p'));
  REQUIRE(loaded->savedAt == 1755645600);
}

TEST_CASE("SessionStore file permissions", "[SessionStore]") {
  if (::geteuid() == 0) {
    WARN("Test running as root: Skipping test");
    return;
  }
  TestEnvironment env;
  const string home = env.setHomeDir(env.createTempDir());

  saveSession(makeInfo("secret"));

  env.requireModeLessPrivilegedThan(home + "/.et", 0700);
  env.requireModeLessPrivilegedThan(home + "/.et/sessions", 0700);
  env.requireModeLessPrivilegedThan(home + "/.et/sessions/secret", 0600);
}

TEST_CASE("SessionStore load missing and invalid names", "[SessionStore]") {
  TestEnvironment env;
  env.setHomeDir(env.createTempDir());

  REQUIRE_FALSE(loadSession("nope").has_value());
  // Invalid names never touch the filesystem.
  REQUIRE_FALSE(loadSession("../escape").has_value());
  REQUIRE_FALSE(loadSession("").has_value());
}

TEST_CASE("SessionStore delete removes file", "[SessionStore]") {
  TestEnvironment env;
  const string home = env.setHomeDir(env.createTempDir());

  saveSession(makeInfo("alpha"));
  REQUIRE(loadSession("alpha").has_value());

  deleteSession("alpha");
  REQUIRE_FALSE(loadSession("alpha").has_value());
  // Deleting a nonexistent session is a no-op.
  deleteSession("alpha");
}

TEST_CASE("SessionStore list is sorted and skips corrupt entries",
          "[SessionStore]") {
  TestEnvironment env;
  const string home = env.setHomeDir(env.createTempDir());

  saveSession(makeInfo("zeta"));
  saveSession(makeInfo("alpha"));
  saveSession(makeInfo("mid"));

  // Corrupt and non-session files must be skipped, not fatal.
  const string dir = home + "/.et/sessions";
  {
    FILE* f = fopen((dir + "/broken").c_str(), "w");
    REQUIRE(f != nullptr);
    fprintf(f, "not a session file\n");
    fclose(f);
  }
  {
    FILE* f = fopen((dir + "/badversion").c_str(), "w");
    REQUIRE(f != nullptr);
    fprintf(f, "version=99\nname=badversion\n");
    fclose(f);
  }

  vector<SessionInfo> sessions = listSessions();
  REQUIRE(sessions.size() == 3);
  REQUIRE(sessions[0].name == "alpha");
  REQUIRE(sessions[1].name == "mid");
  REQUIRE(sessions[2].name == "zeta");
}

TEST_CASE("SessionStore save over existing name replaces atomically",
          "[SessionStore]") {
  TestEnvironment env;
  env.setHomeDir(env.createTempDir());

  saveSession(makeInfo("alpha", "old-host"));
  saveSession(makeInfo("alpha", "new-host"));

  const optional<SessionInfo> loaded = loadSession("alpha");
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->host == "new-host");

  const vector<SessionInfo> sessions = listSessions();
  REQUIRE(sessions.size() == 1);
}

TEST_CASE("SessionStore list on missing directory is empty", "[SessionStore]") {
  TestEnvironment env;
  env.setHomeDir(env.createTempDir());
  REQUIRE(listSessions().empty());
}
