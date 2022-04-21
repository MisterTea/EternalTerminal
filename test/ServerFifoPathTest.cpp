#include <ftw.h>

#include <filesystem>
#include <optional>

#include "ServerFifoPath.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {

struct FileInfo {
  bool exists = false;
  mode_t mode = 0;

  mode_t fileMode() const { return mode & 0777; }

  // Codespaces and similar environments may enforce additional ACLs, so verify
  // that the permissions are less than a certain maximum. See
  // https://github.community/t/bug-umask-does-not-seem-to-be-respected/129638
  void requireFileModeLessPrivilegedThan(mode_t highestMode) const {
    INFO("fileMode()=" << fileMode() << ", highestMode=" << highestMode);
    REQUIRE((fileMode() & highestMode) == fileMode());
  }
};

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
    string tmpPath = GetTempDirectory() + string("et_test_XXXXXXXX");
    const string dir = string(mkdtemp(&tmpPath[0]));

    temporaryDirs.push_back(dir);
    return dir;
  }

  FileInfo getFileInfo(const string& name) {
    struct stat fileStat;
    const int statResult = ::stat(name.c_str(), &fileStat);
    if (statResult != 0) {
      return FileInfo{};
    }

    FileInfo result;
    result.exists = true;
    result.mode = fileStat.st_mode;
    return result;
  }

  void setEnv(const char* name, const string& value) {
    if (!savedEnvs.count(name)) {
      const char* previousValue = ::getenv(name);
      if (previousValue) {
        savedEnvs[name] = string(previousValue);
      } else {
        savedEnvs[name] = std::nullopt;
      }
    }

    const int replace = 1;  // non-zero to replace.
    ::setenv(name, value.c_str(), replace);
  }

  ~TestEnvironment() {
    // Remove temporary dirs.
    for (const string& dir : temporaryDirs) {
      const int removeResult = RemoveDirectory(dir.c_str());
      if (removeResult == -1) {
        LOG(ERROR) << "Error when removing dir: " << dir;
        FATAL_FAIL(removeResult);
      }
    }

    // Restore env.
    for (const auto& [key, value] : savedEnvs) {
      if (value) {
        const int replace = 1;  // non-zero to replace.
        ::setenv(key.c_str(), value->c_str(), replace);
      } else {
        ::unsetenv(key.c_str());
      }
    }
  }

 private:
  vector<string> temporaryDirs;
  map<string, optional<string>> savedEnvs;
};

}  // namespace

TEST_CASE("Creation", "[ServerFifoPath]") {
  TestEnvironment env;

  const string homeDir = env.createTempDir();
  env.setEnv("HOME", homeDir.c_str());
  INFO("homeDir = " << homeDir);

  const string expectedFifoPath =
      homeDir + "/.local/share/etserver/etserver.idpasskey.fifo";

  ServerFifoPath serverFifo;
  REQUIRE(serverFifo.getPathForCreation() == expectedFifoPath);
  REQUIRE(serverFifo.getEndpointForConnect() ==
          std::nullopt);  // Expected to be null unless the path is overridden.

  SECTION("Create all directories") {
    REQUIRE(!env.getFileInfo(homeDir + "/.local/share/etserver").exists);
    serverFifo.createDirectoriesIfRequired();

    // Verify the entire tree is created with the correct permissions.
    env.getFileInfo(homeDir + "/.local")
        .requireFileModeLessPrivilegedThan(0755);
    env.getFileInfo(homeDir + "/.local/share")
        .requireFileModeLessPrivilegedThan(0755);
    env.getFileInfo(homeDir + "/.local/share/etserver")
        .requireFileModeLessPrivilegedThan(0700);
  }

  const string localDir = homeDir + "/.local";
  const mode_t localDirMode = 0777;  // Create with different permissions so
                                     // we can check that this hasn't changed.
  const string shareDir = homeDir + "/.local/share";
  const mode_t shareDirMode = 0770;  // Another non-default mode.
  const string etserverDir = homeDir + "/.local/share/etserver";

  SECTION(".local already exists") {
    const int oldMask = ::umask(0);
    FATAL_FAIL(::mkdir(localDir.c_str(), localDirMode));
    ::umask(oldMask);

    serverFifo.createDirectoriesIfRequired();

    env.getFileInfo(homeDir + "/.local")
        .requireFileModeLessPrivilegedThan(localDirMode);
    env.getFileInfo(homeDir + "/.local/share")
        .requireFileModeLessPrivilegedThan(0755);
    env.getFileInfo(homeDir + "/.local/share/etserver")
        .requireFileModeLessPrivilegedThan(0700);
  }

  SECTION(".local/share already exists") {
    const int oldMask = ::umask(0);
    FATAL_FAIL(::mkdir(localDir.c_str(), localDirMode));
    FATAL_FAIL(::mkdir(shareDir.c_str(), shareDirMode));
    ::umask(oldMask);

    serverFifo.createDirectoriesIfRequired();

    env.getFileInfo(homeDir + "/.local")
        .requireFileModeLessPrivilegedThan(localDirMode);
    env.getFileInfo(homeDir + "/.local/share")
        .requireFileModeLessPrivilegedThan(shareDirMode);
    env.getFileInfo(homeDir + "/.local/share/etserver")
        .requireFileModeLessPrivilegedThan(0700);
  }

  SECTION(".local/share/etserver already exists") {
    const mode_t etserverDirMode = 0750;  // Use slightly different permissions,
                                          // but still without write access.

    const int oldMask = ::umask(0);
    FATAL_FAIL(::mkdir(localDir.c_str(), localDirMode));
    FATAL_FAIL(::mkdir(shareDir.c_str(), shareDirMode));
    FATAL_FAIL(::mkdir(etserverDir.c_str(), etserverDirMode));
    ::umask(oldMask);

    serverFifo.createDirectoriesIfRequired();

    env.getFileInfo(homeDir + "/.local")
        .requireFileModeLessPrivilegedThan(localDirMode);
    env.getFileInfo(homeDir + "/.local/share")
        .requireFileModeLessPrivilegedThan(shareDirMode);
    env.getFileInfo(homeDir + "/.local/share/etserver")
        .requireFileModeLessPrivilegedThan(etserverDirMode);
  }

  SECTION("Override XDG_RUNTIME_DIR") {
    const string xdgRuntimeDir = env.createTempDir();
    env.setEnv("XDG_RUNTIME_DIR", xdgRuntimeDir);

    const string xdgRuntimeDirFifoPath =
        xdgRuntimeDir + "/etserver/etserver.idpasskey.fifo";
    REQUIRE(serverFifo.getPathForCreation() == xdgRuntimeDirFifoPath);

    // Test creation of the etserver subdirectory.
    const string xdgRuntimeDirEtserver = xdgRuntimeDir + "/etserver";
    REQUIRE(!env.getFileInfo(xdgRuntimeDirEtserver).exists);

    serverFifo.createDirectoriesIfRequired();

    env.getFileInfo(xdgRuntimeDirEtserver)
        .requireFileModeLessPrivilegedThan(0700);
  }
}

TEST_CASE("Override", "[ServerFifoPath]") {
  TestEnvironment env;

  const string homeDir = env.createTempDir();
  env.setEnv("HOME", homeDir.c_str());

  const string expectedFifoPath =
      homeDir + "/.local/share/etserver/etserver.idpasskey.fifo";

  ServerFifoPath serverFifo;
  REQUIRE(serverFifo.getPathForCreation() == expectedFifoPath);
  REQUIRE(serverFifo.getEndpointForConnect() == std::nullopt);

  // Override and re-test.
  const string pathOverride = env.createTempDir() + "/etserver.idpasskey.fifo";
  serverFifo.setPathOverride(pathOverride);

  REQUIRE(serverFifo.getPathForCreation() == pathOverride);

  const optional<SocketEndpoint> endpoint = serverFifo.getEndpointForConnect();
  REQUIRE(endpoint != std::nullopt);
  REQUIRE(endpoint.value().name() == pathOverride);
}
