#include "TestHeaders.hpp"
#include "UserSocketOps.hpp"

#ifndef WIN32
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace et;

namespace {
string makeTempDir() {
  string dirTemplate = GetTempDirectory() + "et_user_sock_XXXXXX";
  return string(mkdtemp(&dirTemplate[0]));
}

string longUnixPath() {
  // sun_path is typically 108 bytes including NUL.
  return string(sizeof(sockaddr_un::sun_path) + 8, 'x');
}
}  // namespace

TEST_CASE("UserSocketOps listen and connect as current user",
          "[UserSocketOps]") {
  string dir = makeTempDir();
  string path = dir + "/sock";

  uid_t uid = getuid();
  gid_t gid = getgid();

  int listenFd = UserSocketOps::listenUnixAsUser(path, uid, gid);
  REQUIRE(listenFd >= 0);

  struct stat st;
  REQUIRE(::stat(path.c_str(), &st) == 0);
  REQUIRE(S_ISSOCK(st.st_mode));

  int connFd = UserSocketOps::connectUnixAsUser(path, uid, gid);
  REQUIRE(connFd >= 0);

  int client = ::accept(listenFd, nullptr, nullptr);
  REQUIRE(client >= 0);

  REQUIRE(::write(connFd, "ping", 4) == 4);
  char buf[4];
  REQUIRE(::read(client, buf, 4) == 4);
  REQUIRE(string(buf, 4) == "ping");
  REQUIRE(::write(client, "pong", 4) == 4);
  REQUIRE(::read(connFd, buf, 4) == 4);
  REQUIRE(string(buf, 4) == "pong");

  ::close(client);
  ::close(connFd);
  ::close(listenFd);
  ::unlink(path.c_str());
  ::rmdir(dir.c_str());
}

TEST_CASE("UserSocketOps listenAtPath and connectAtPath in-process",
          "[UserSocketOps]") {
  string dir = makeTempDir();
  string path = dir + "/sock";

  int listenFd = UserSocketOps::listenAtPath(path);
  REQUIRE(listenFd >= 0);

  struct stat st;
  REQUIRE(::stat(path.c_str(), &st) == 0);
  REQUIRE(S_ISSOCK(st.st_mode));

  int connFd = UserSocketOps::connectAtPath(path);
  REQUIRE(connFd >= 0);

  int client = ::accept(listenFd, nullptr, nullptr);
  REQUIRE(client >= 0);
  REQUIRE(::write(connFd, "ok", 2) == 2);
  char buf[2];
  REQUIRE(::read(client, buf, 2) == 2);
  REQUIRE(string(buf, 2) == "ok");

  ::close(client);
  ::close(connFd);
  ::close(listenFd);
  ::unlink(path.c_str());
  ::rmdir(dir.c_str());
}

TEST_CASE("UserSocketOps listenAtPath rejects oversized path",
          "[UserSocketOps]") {
  int fd = UserSocketOps::listenAtPath(longUnixPath());
  REQUIRE(fd < 0);
  REQUIRE(GetErrno() == ENAMETOOLONG);
}

TEST_CASE("UserSocketOps connectAtPath rejects oversized path",
          "[UserSocketOps]") {
  int fd = UserSocketOps::connectAtPath(longUnixPath());
  REQUIRE(fd < 0);
  REQUIRE(GetErrno() == ENAMETOOLONG);
}

TEST_CASE("UserSocketOps listenUnixAsUser rejects oversized path",
          "[UserSocketOps]") {
  int fd = UserSocketOps::listenUnixAsUser(longUnixPath(), getuid(), getgid());
  REQUIRE(fd < 0);
  REQUIRE(GetErrno() == ENAMETOOLONG);
}

TEST_CASE("UserSocketOps connectUnixAsUser rejects oversized path",
          "[UserSocketOps]") {
  int fd = UserSocketOps::connectUnixAsUser(longUnixPath(), getuid(), getgid());
  REQUIRE(fd < 0);
  REQUIRE(GetErrno() == ENAMETOOLONG);
}

TEST_CASE("UserSocketOps listenAtPath fails when path is a directory",
          "[UserSocketOps]") {
  string dir = makeTempDir();
  // Path is itself a directory: unlink fails, bind must fail.
  int fd = UserSocketOps::listenAtPath(dir);
  REQUIRE(fd < 0);

  ::rmdir(dir.c_str());
}

TEST_CASE("UserSocketOps connectAtPath fails when nothing listens",
          "[UserSocketOps]") {
  string dir = makeTempDir();
  string path = dir + "/missing";
  int fd = UserSocketOps::connectAtPath(path);
  REQUIRE(fd < 0);

  ::rmdir(dir.c_str());
}

TEST_CASE("UserSocketOps connectUnixAsUser fails when nothing listens",
          "[UserSocketOps]") {
  string dir = makeTempDir();
  string path = dir + "/missing";
  int fd = UserSocketOps::connectUnixAsUser(path, getuid(), getgid());
  REQUIRE(fd < 0);

  ::rmdir(dir.c_str());
}

TEST_CASE("UserSocketOps listen as user cannot unlink root-only path",
          "[UserSocketOps]") {
  if (getuid() == 0) {
    SKIP("Test requires a non-root process");
  }

  // A path under /dev that a normal user cannot replace.
  string path = "/dev/null_et_should_not_bind";
  int fd = UserSocketOps::listenUnixAsUser(path, getuid(), getgid());
  REQUIRE(fd < 0);
}

TEST_CASE("UserSocketOps listenAtPath replaces an existing socket path",
          "[UserSocketOps]") {
  string dir = makeTempDir();
  string path = dir + "/sock";

  int first = UserSocketOps::listenAtPath(path);
  REQUIRE(first >= 0);
  ::close(first);

  int second = UserSocketOps::listenAtPath(path);
  REQUIRE(second >= 0);
  ::close(second);
  ::unlink(path.c_str());
  ::rmdir(dir.c_str());
}
#endif
