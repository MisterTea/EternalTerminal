#ifndef WIN32
#include "ETerminal.pb.h"
#include "PipeSocketHandler.hpp"
#include "TestHeaders.hpp"
#include "UserTerminalRouter.hpp"

using namespace et;

TEST_CASE("UserTerminalRouter constructor creates server",
          "[UserTerminalRouter]") {
  auto socketHandler = std::make_shared<PipeSocketHandler>();

  string tmpPath = GetTempDirectory() + string("et_test_router_ctor_XXXXXXXX");
  string pipeDirectory = string(mkdtemp(&tmpPath[0]));
  string pipePath = pipeDirectory + "/router_pipe";

  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(pipePath);

  UserTerminalRouter router(socketHandler, routerEndpoint);

  // Verify that the server fd was created
  REQUIRE(router.getServerFd() >= 0);

  // Verify that the pipe file was created with correct permissions
  struct stat st;
  REQUIRE(stat(pipePath.c_str(), &st) == 0);
  // Check that the file has read/write/execute for user, group, and others
  REQUIRE((st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != 0);

  socketHandler->close(router.getServerFd());
  FATAL_FAIL(::remove(pipePath.c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

TEST_CASE("UserTerminalRouter acceptNewConnection with no client",
          "[UserTerminalRouter]") {
  auto socketHandler = std::make_shared<PipeSocketHandler>();

  string tmpPath =
      GetTempDirectory() + string("et_test_router_noaccept_XXXXXXXX");
  string pipeDirectory = string(mkdtemp(&tmpPath[0]));
  string pipePath = pipeDirectory + "/router_pipe";

  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(pipePath);

  UserTerminalRouter router(socketHandler, routerEndpoint);

  // Try to accept without any client connecting - should return empty pair
  IdKeyPair result = router.acceptNewConnection();

  REQUIRE(result.id == "");
  REQUIRE(result.key == "");

  socketHandler->close(router.getServerFd());
  FATAL_FAIL(::remove(pipePath.c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

TEST_CASE("UserTerminalRouter getSocketHandler returns handler",
          "[UserTerminalRouter]") {
  auto socketHandler = std::make_shared<PipeSocketHandler>();

  string tmpPath =
      GetTempDirectory() + string("et_test_router_getsock_XXXXXXXX");
  string pipeDirectory = string(mkdtemp(&tmpPath[0]));
  string pipePath = pipeDirectory + "/router_pipe";

  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(pipePath);

  UserTerminalRouter router(socketHandler, routerEndpoint);

  REQUIRE(router.getSocketHandler() == socketHandler);

  socketHandler->close(router.getServerFd());
  FATAL_FAIL(::remove(pipePath.c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

#endif
