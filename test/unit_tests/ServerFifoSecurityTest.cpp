#include "ServerFifoPath.hpp"
#include "TestHeaders.hpp"

#ifndef WIN32
using namespace et;

namespace {
struct FifoSandbox {
  string dir = test::makeTempDir("et_fifo_security");
  optional<string> saved;
  FifoSandbox() {
    const char* value = getenv("XDG_RUNTIME_DIR");
    if (value) saved = value;
    setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
  }
  ~FifoSandbox() {
    if (saved) {
      setenv("XDG_RUNTIME_DIR", saved->c_str(), 1);
    } else {
      unsetenv("XDG_RUNTIME_DIR");
    }
    fs::remove_all(dir);
  }
};

bool refusedDirectory() {
  const pid_t child = fork();
  if (child < 0) throw std::runtime_error("fork failed");
  if (child == 0) {
    try {
      ServerFifoPath().createDirectoriesIfRequired();
      _exit(0);
    } catch (...) {
      _exit(1);
    }
  }
  int status = 0;
  if (waitpid(child, &status, 0) != child) {
    throw std::runtime_error("waitpid failed");
  }
  return !WIFEXITED(status) || WEXITSTATUS(status) != 0;
}

// True when gid is the user's private group: named after the user, the
// user's primary group, and listing no other member.
bool isUserPrivateGroup(gid_t gid) {
  const passwd* user = getpwuid(geteuid());
  const group* entry = getgrgid(gid);
  if (user == nullptr || entry == nullptr || user->pw_gid != gid ||
      string(entry->gr_name) != user->pw_name) {
    return false;
  }
  for (char** member = entry->gr_mem; *member != nullptr; ++member) {
    if (string(*member) != user->pw_name) return false;
  }
  return true;
}
}  // namespace

TEST_CASE("Server fifo refuses a directory replaceable through its parent",
          "[ServerFifoPath][security]") {
  if (geteuid() == 0) SKIP("Exercises the ordinary-user server path");
  FifoSandbox sandbox;
  REQUIRE(mkdir((sandbox.dir + "/etserver").c_str(), 0700) == 0);
  REQUIRE(chmod(sandbox.dir.c_str(), 0777) == 0);
  // A private child can still be renamed/replaced through a writable parent.
  CHECK(refusedDirectory());
}

TEST_CASE("Server fifo accepts a parent writable by the user private group",
          "[ServerFifoPath][security]") {
  if (geteuid() == 0) SKIP("Exercises the ordinary-user server path");
  const passwd* user = getpwuid(geteuid());
  REQUIRE(user != nullptr);
  if (!isUserPrivateGroup(user->pw_gid)) SKIP("No user private group");
  FifoSandbox sandbox;
  // umask 002 on user-private-group systems leaves directories like this.
  REQUIRE(chown(sandbox.dir.c_str(), -1, user->pw_gid) == 0);
  REQUIRE(chmod(sandbox.dir.c_str(), 0770) == 0);
  CHECK_FALSE(refusedDirectory());
}

TEST_CASE("Server fifo refuses a parent writable by a shared group",
          "[ServerFifoPath][security]") {
  if (geteuid() == 0) SKIP("Exercises the ordinary-user server path");
  vector<gid_t> groups(getgroups(0, nullptr));
  groups.resize(getgroups(groups.size(), groups.data()));
  const auto shared = std::find_if(groups.begin(), groups.end(), [](gid_t gid) {
    return !isUserPrivateGroup(gid);
  });
  if (shared == groups.end()) SKIP("No shared group to test with");
  FifoSandbox sandbox;
  REQUIRE(chown(sandbox.dir.c_str(), -1, *shared) == 0);
  REQUIRE(chmod(sandbox.dir.c_str(), 0770) == 0);
  CHECK(refusedDirectory());
}

TEST_CASE("Server fifo refuses a symlinked private directory",
          "[ServerFifoPath][security]") {
  if (geteuid() == 0) SKIP("Exercises the ordinary-user server path");
  FifoSandbox sandbox;
  const string victim = sandbox.dir + "/victim";
  REQUIRE(mkdir(victim.c_str(), 0700) == 0);
  REQUIRE(symlink(victim.c_str(), (sandbox.dir + "/etserver").c_str()) == 0);
  CHECK(refusedDirectory());
  CHECK(fs::is_empty(victim));
}
#endif
