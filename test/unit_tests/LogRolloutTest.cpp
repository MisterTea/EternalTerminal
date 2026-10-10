#include "LogHandler.hpp"
#include "TestHeaders.hpp"

using namespace et;
using namespace et::test;

#ifndef WIN32
TEST_CASE("Log rollout handler leaves the log file in place", "[LogHandler]") {
  const string dir = makeTempDir("et_rollout_test");
  const string logPath = dir + "/et.log";
  const int fd = ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  REQUIRE(fd >= 0);
  ::close(fd);
  struct stat before;
  REQUIRE(stat(logPath.c_str(), &before) == 0);

  LogHandler::rolloutHandler(logPath.c_str(), 0);

  struct stat after;
  REQUIRE(stat(logPath.c_str(), &after) == 0);
  REQUIRE(after.st_ino == before.st_ino);
  ::remove(logPath.c_str());
  removeTempDir(dir);
}
#endif
