#include "TestHeaders.hpp"
#include "UserSocketOps.hpp"

#ifndef WIN32
#include <sys/stat.h>
#include <unistd.h>

using namespace et;

TEST_CASE("UserSocketOps listen and connect as current user",
          "[UserSocketOps]") {
  string dirTemplate = GetTempDirectory() + "et_user_sock_XXXXXX";
  string dir = string(mkdtemp(&dirTemplate[0]));
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
#endif
