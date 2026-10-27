#include "TestHeaders.hpp"
#include "UserSocketOps.hpp"

#ifdef WIN32
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace et;
using namespace et::test;

namespace {
string makeTempDir() { return test::makeTempDir("et_user_sock"); }

string longUnixPath() {
  // sun_path is typically 108 bytes including NUL.
  return string(sizeof(sockaddr_un::sun_path) + 8, 'x');
}

uid_t currentUid() {
#ifdef WIN32
  return 0;
#else
  return getuid();
#endif
}

gid_t currentGid() {
#ifdef WIN32
  return 0;
#else
  return getgid();
#endif
}

bool socketFileExists(const string& path) {
#ifdef WIN32
  return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) {
    return false;
  }
  return S_ISSOCK(st.st_mode);
#endif
}

ssize_t socketWrite(int fd, const void* buf, size_t count) {
#ifdef WIN32
  return ::send(fd, static_cast<const char*>(buf), static_cast<int>(count), 0);
#else
  return ::write(fd, buf, count);
#endif
}

ssize_t socketRead(int fd, void* buf, size_t count) {
#ifdef WIN32
  return ::recv(fd, static_cast<char*>(buf), static_cast<int>(count), 0);
#else
  return ::read(fd, buf, count);
#endif
}

int socketAccept(int listenFd) {
#ifdef WIN32
  SOCKET client = ::accept(static_cast<SOCKET>(listenFd), nullptr, nullptr);
  if (client == INVALID_SOCKET) {
    return -1;
  }
  return static_cast<int>(client);
#else
  return ::accept(listenFd, nullptr, nullptr);
#endif
}

void removeSocketPath(const string& path) {
#ifdef WIN32
  _unlink(path.c_str());
#else
  ::unlink(path.c_str());
#endif
}
}  // namespace

TEST_CASE("UserSocketOps listen and connect as current user",
          "[UserSocketOps]") {
  string dir = makeTempDir();
  string path = dir + "/sock";

  uid_t uid = currentUid();
  gid_t gid = currentGid();

  int listenFd = UserSocketOps::listenUnixAsUser(path, uid, gid);
  REQUIRE(listenFd >= 0);

  REQUIRE(socketFileExists(path));

  int connFd = UserSocketOps::connectUnixAsUser(path, uid, gid);
  REQUIRE(connFd >= 0);

  int client = socketAccept(listenFd);
  REQUIRE(client >= 0);

  REQUIRE(socketWrite(connFd, "ping", 4) == 4);
  char buf[4];
  REQUIRE(socketRead(client, buf, 4) == 4);
  REQUIRE(string(buf, 4) == "ping");
  REQUIRE(socketWrite(client, "pong", 4) == 4);
  REQUIRE(socketRead(connFd, buf, 4) == 4);
  REQUIRE(string(buf, 4) == "pong");

  test::closeTestFd(client);
  test::closeTestFd(connFd);
  test::closeTestFd(listenFd);
  removeSocketPath(path);
  test::removeTempDir(dir);
}

TEST_CASE("UserSocketOps listenAtPath and connectAtPath in-process",
          "[UserSocketOps]") {
  string dir = makeTempDir();
  string path = dir + "/sock";

  int listenFd = UserSocketOps::listenAtPath(path);
  REQUIRE(listenFd >= 0);

  REQUIRE(socketFileExists(path));

  int connFd = UserSocketOps::connectAtPath(path);
  REQUIRE(connFd >= 0);

  int client = socketAccept(listenFd);
  REQUIRE(client >= 0);
  REQUIRE(socketWrite(connFd, "ok", 2) == 2);
  char buf[2];
  REQUIRE(socketRead(client, buf, 2) == 2);
  REQUIRE(string(buf, 2) == "ok");

  test::closeTestFd(client);
  test::closeTestFd(connFd);
  test::closeTestFd(listenFd);
  removeSocketPath(path);
  test::removeTempDir(dir);
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
  int fd = UserSocketOps::listenUnixAsUser(longUnixPath(), currentUid(),
                                           currentGid());
  REQUIRE(fd < 0);
  REQUIRE(GetErrno() == ENAMETOOLONG);
}

TEST_CASE("UserSocketOps connectUnixAsUser rejects oversized path",
          "[UserSocketOps]") {
  int fd = UserSocketOps::connectUnixAsUser(longUnixPath(), currentUid(),
                                            currentGid());
  REQUIRE(fd < 0);
  REQUIRE(GetErrno() == ENAMETOOLONG);
}

TEST_CASE("UserSocketOps listenAtPath fails when path is a directory",
          "[UserSocketOps]") {
  string dir = makeTempDir();
  // Path is itself a directory: unlink fails, bind must fail.
  int fd = UserSocketOps::listenAtPath(dir);
  REQUIRE(fd < 0);

  test::removeTempDir(dir);
}

TEST_CASE("UserSocketOps connectAtPath fails when nothing listens",
          "[UserSocketOps]") {
  string dir = makeTempDir();
  string path = dir + "/missing";
  int fd = UserSocketOps::connectAtPath(path);
  REQUIRE(fd < 0);

  test::removeTempDir(dir);
}

TEST_CASE("UserSocketOps connectUnixAsUser fails when nothing listens",
          "[UserSocketOps]") {
  string dir = makeTempDir();
  string path = dir + "/missing";
  int fd = UserSocketOps::connectUnixAsUser(path, currentUid(), currentGid());
  REQUIRE(fd < 0);

  test::removeTempDir(dir);
}

TEST_CASE("UserSocketOps listen as user cannot unlink privileged path",
          "[UserSocketOps]") {
#ifndef WIN32
  if (getuid() == 0) {
    SKIP("Test requires a non-root process");
  }

  // A path under /dev that a normal user cannot replace.
  string path = "/dev/null_et_should_not_bind";
#else
  // No privilege separation on Windows; a path under a directory that does
  // not exist cannot be bound by anyone.
  string path = "et_no_such_dir_xyzzy/null_et_should_not_bind";
#endif
  int fd = UserSocketOps::listenUnixAsUser(path, currentUid(), currentGid());
  REQUIRE(fd < 0);
}

TEST_CASE("UserSocketOps listenAtPath replaces an existing socket path",
          "[UserSocketOps]") {
  string dir = makeTempDir();
  string path = dir + "/sock";

  int first = UserSocketOps::listenAtPath(path);
  REQUIRE(first >= 0);
  test::closeTestFd(first);

  int second = UserSocketOps::listenAtPath(path);
  REQUIRE(second >= 0);
  test::closeTestFd(second);
  removeSocketPath(path);
  test::removeTempDir(dir);
}
