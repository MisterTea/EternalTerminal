#include <ftw.h>
#include <utime.h>

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
  info.title = "";
  info.savedAt = 1755645600;
  info.lastSeenAt = 0;
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
  SessionInfo titledInfo = info;
  titledInfo.title = "Claude Code - router recovery";
  saveSession(titledInfo);

  const optional<SessionInfo> loaded = loadSession("alpha");
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->name == "alpha");
  REQUIRE(loaded->host == "10.0.0.5");
  REQUIRE(loaded->port == 9922);
  REQUIRE(loaded->id == "id-abc");
  REQUIRE(loaded->passkey == string(32, 'p'));
  REQUIRE(loaded->title == "Claude Code - router recovery");
  REQUIRE(loaded->savedAt == 1755645600);
  REQUIRE(loaded->lastSeenAt > 0);
}

TEST_CASE("SessionStore loads version 1 files without a title",
          "[SessionStore]") {
  TestEnvironment env;
  const string home = env.setHomeDir(env.createTempDir());
  const string dir = home + "/.et/sessions";
  REQUIRE(std::filesystem::create_directories(dir));
#ifndef WIN32
  REQUIRE(::chmod((home + "/.et").c_str(), 0700) == 0);
  REQUIRE(::chmod(dir.c_str(), 0700) == 0);
#endif

  FILE* f = fopen((dir + "/legacy").c_str(), "w");
  REQUIRE(f != nullptr);
  fprintf(f,
          "version=1\nname=legacy\nhost=nas\nport=2022\nid=old-id\n"
          "passkey=kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk\nsavedat=1755645600\n");
  fclose(f);
#ifndef WIN32
  REQUIRE(::chmod((dir + "/legacy").c_str(), 0600) == 0);
#endif

  const optional<SessionInfo> loaded = loadSession("legacy");
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->title.empty());
}

TEST_CASE("SessionStore updates only the saved title", "[SessionStore]") {
  TestEnvironment env;
  env.setHomeDir(env.createTempDir());
  const SessionInfo original =
      makeInfo("alpha", "10.0.0.5", 9922, "id-abc", string(32, 'p'));
  saveSession(original);

  REQUIRE(updateSessionTitle("alpha", "new title"));
  const optional<SessionInfo> loaded = loadSession("alpha");
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->title == "new title");
  REQUIRE(loaded->host == original.host);
  REQUIRE(loaded->port == original.port);
  REQUIRE(loaded->id == original.id);
  REQUIRE(loaded->passkey == original.passkey);
  REQUIRE(loaded->savedAt == original.savedAt);
  REQUIRE_FALSE(updateSessionTitle("missing", "title"));
}

TEST_CASE("SessionStore touch updates last seen time", "[SessionStore]") {
  TestEnvironment env;
  const string home = env.setHomeDir(env.createTempDir());
  const string path = home + "/.et/sessions/alpha";

  saveSession(makeInfo("alpha"));
  const time_t oldTime = time(nullptr) - 300;
  struct utimbuf oldTimes = {oldTime, oldTime};
  REQUIRE(::utime(path.c_str(), &oldTimes) == 0);

  const optional<SessionInfo> oldSession = loadSession("alpha");
  REQUIRE(oldSession.has_value());
  REQUIRE(oldSession->lastSeenAt == oldTime);
  REQUIRE(touchSession("alpha"));

  const optional<SessionInfo> touchedSession = loadSession("alpha");
  REQUIRE(touchedSession.has_value());
  REQUIRE(touchedSession->lastSeenAt > oldSession->lastSeenAt);
  REQUIRE(touchedSession->savedAt == oldSession->savedAt);
  REQUIRE_FALSE(touchSession("missing"));
  REQUIRE_FALSE(touchSession("../invalid"));
}

TEST_CASE("SessionStore formats relative last seen times", "[SessionStore]") {
  const int64_t now = 1'000'000;
  REQUIRE(formatLastSeen(now, now) == "now");
  REQUIRE(formatLastSeen(now - 30, now) == "now");
  REQUIRE(formatLastSeen(now - 45, now) == "45s ago");
  REQUIRE(formatLastSeen(now - 5 * 60, now) == "5m ago");
  REQUIRE(formatLastSeen(now - 3 * 60 * 60, now) == "3h ago");
  REQUIRE(formatLastSeen(now - 2 * 24 * 60 * 60, now) == "2d ago");
  REQUIRE(formatLastSeen(now + 10, now) == "now");
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

TEST_CASE("SessionStore creates credential files with mode 0600",
          "[SessionStore]") {
  TestEnvironment env;
  const string home = env.setHomeDir(env.createTempDir());
  const mode_t previousUmask = ::umask(0022);

  saveSession(makeInfo("secret"));

  ::umask(previousUmask);
  REQUIRE(env.fileMode(home + "/.et/sessions/secret") == 0600);
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

TEST_CASE("SessionStore reports deletion failures without exposing credentials",
          "[SessionStore]") {
  TestEnvironment env;
  const string home = env.setHomeDir(env.createTempDir());
  const string directory = home + "/.et/sessions";
  const string path = directory + "/alpha";
  const SessionInfo info = makeInfo("alpha");
  saveSession(info);

  SECTION("unsafe parent is not reported as a successful deletion") {
    REQUIRE(::chmod(directory.c_str(), 0750) == 0);
    bool failed = false;
    try {
      deleteSession("alpha");
    } catch (const std::exception& error) {
      failed = true;
      CHECK(string(error.what()).find(info.passkey) == string::npos);
    }
    REQUIRE(::chmod(directory.c_str(), 0700) == 0);
    REQUIRE(failed);
    REQUIRE(loadSession("alpha").has_value());
  }

  SECTION("unlink permission errors reach the caller") {
    if (getuid() == 0) {
      return;  // Root can unlink despite directory permissions.
    }
    REQUIRE(::chmod(directory.c_str(), 0500) == 0);
    bool failed = false;
    try {
      deleteSession("alpha");
    } catch (const std::exception& error) {
      failed = true;
      CHECK(string(error.what()).find(info.passkey) == string::npos);
    }
    REQUIRE(::chmod(directory.c_str(), 0700) == 0);
    REQUIRE(failed);
    REQUIRE(loadSession("alpha").has_value());
  }

  SECTION("unsafe records are retained with an explicit failure") {
    REQUIRE(::chmod(path.c_str(), 0640) == 0);
    REQUIRE_THROWS(deleteSession("alpha"));
    struct stat fileStat;
    REQUIRE(::lstat(path.c_str(), &fileStat) == 0);
  }
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
  {
    FILE* f = fopen((dir + "/badport").c_str(), "w");
    REQUIRE(f != nullptr);
    fprintf(f,
            "version=1\nname=badport\nhost=nas\nport=notanumber\n"
            "id=id\npasskey=key\nsavedat=1755645600\n");
    fclose(f);
  }
  {
    FILE* f = fopen((dir + "/truncated").c_str(), "w");
    REQUIRE(f != nullptr);
    fprintf(f,
            "version=1\nname=truncated\nhost=nas\nport=2022\n"
            "id=id\npasskey=key\nsavedat=");
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

TEST_CASE("SessionStore can refuse to replace an existing record",
          "[SessionStore]") {
  TestEnvironment env;
  env.setHomeDir(env.createTempDir());

  saveSession(makeInfo("alpha", "old-host", 2022, "old-id", "old-passkey"));
  REQUIRE_THROWS(saveSession(
      makeInfo("alpha", "new-host", 2023, "new-id", "new-passkey"), false));

  const optional<SessionInfo> loaded = loadSession("alpha");
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->host == "old-host");
  REQUIRE(loaded->port == 2022);
  REQUIRE(loaded->id == "old-id");
  REQUIRE(loaded->passkey == "old-passkey");
}

TEST_CASE("SessionStore list on missing directory is empty", "[SessionStore]") {
  TestEnvironment env;
  env.setHomeDir(env.createTempDir());
  REQUIRE(listSessions().empty());
}

#ifndef WIN32
TEST_CASE("SessionStore rejects symlinked storage paths", "[SessionStore]") {
  TestEnvironment env;
  const string home = env.setHomeDir(env.createTempDir());
  const string realDir = env.createTempDir();
  REQUIRE(::symlink(realDir.c_str(), (home + "/.et").c_str()) == 0);

  REQUIRE_FALSE(loadSession("alpha").has_value());
  REQUIRE(listSessions().empty());
  REQUIRE_THROWS(saveSession(makeInfo("alpha")));
}

TEST_CASE("SessionStore rejects unsafe credential permissions",
          "[SessionStore]") {
  if (::geteuid() == 0) {
    WARN("Test running as root: Skipping test");
    return;
  }
  TestEnvironment env;
  const string home = env.setHomeDir(env.createTempDir());
  saveSession(makeInfo("alpha"));

  const string path = home + "/.et/sessions/alpha";
  REQUIRE(::chmod(path.c_str(), 0640) == 0);
  REQUIRE_FALSE(loadSession("alpha").has_value());
  REQUIRE_FALSE(touchSession("alpha"));
  REQUIRE(listSessions().empty());
}

TEST_CASE("SessionStore rejects hard-linked credential files",
          "[SessionStore]") {
  if (::geteuid() == 0) {
    WARN("Test running as root: Skipping test");
    return;
  }
  TestEnvironment env;
  const string home = env.setHomeDir(env.createTempDir());
  saveSession(makeInfo("alpha"));

  const string path = home + "/.et/sessions/alpha";
  const string linkPath = home + "/.et/sessions/alpha-copy";
  REQUIRE(::link(path.c_str(), linkPath.c_str()) == 0);
  REQUIRE_FALSE(loadSession("alpha").has_value());
  REQUIRE_FALSE(touchSession("alpha"));
  REQUIRE_THROWS(saveSession(makeInfo("alpha")));
}
#endif
