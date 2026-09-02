#include "ETerminal.pb.h"
#include "PipeSocketHandler.hpp"
#include "TestHeaders.hpp"
#include "UserTerminalRouter.hpp"

using namespace et;

namespace {
struct RouterEndpoint {
  string directory;
  string path;

  RouterEndpoint() {
#ifdef WIN32
    path = "et_test_router_" + genRandomAlphaNum(12) + ".ipc";
#else
    string tmpPath = GetTempDirectory() + string("et_test_router_XXXXXXXX");
    directory = string(mkdtemp(&tmpPath[0]));
    path = directory + "/router_pipe";
#endif
  }

  void cleanup(const shared_ptr<PipeSocketHandler>& socketHandler) {
    SocketEndpoint endpoint;
    endpoint.set_name(path);
    socketHandler->stopListening(endpoint);
    removeOrMissing(path);
#ifndef WIN32
    FATAL_FAIL(::remove(directory.c_str()));
#endif
  }
};
}  // namespace

TEST_CASE("UserTerminalRouter constructor creates server",
          "[UserTerminalRouter]") {
  auto socketHandler = std::make_shared<PipeSocketHandler>();

  RouterEndpoint testEndpoint;

  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(testEndpoint.path);

  UserTerminalRouter router(socketHandler, routerEndpoint);

  // Verify that the server fd was created
  REQUIRE(router.getServerFd() >= 0);

#ifndef WIN32
  // Verify that the pipe file was created with correct permissions
  struct stat st;
  REQUIRE(stat(testEndpoint.path.c_str(), &st) == 0);
  // Check that the file has read/write/execute for user, group, and others
  REQUIRE((st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != 0);
#endif

  testEndpoint.cleanup(socketHandler);
}

TEST_CASE("UserTerminalRouter acceptNewConnection with no client",
          "[UserTerminalRouter]") {
  auto socketHandler = std::make_shared<PipeSocketHandler>();

  RouterEndpoint testEndpoint;

  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(testEndpoint.path);

  UserTerminalRouter router(socketHandler, routerEndpoint);

  // Try to accept without any client connecting - should return empty pair
  IdKeyPair result = router.acceptNewConnection();

  REQUIRE(result.id == "");
  REQUIRE(result.key == "");

  testEndpoint.cleanup(socketHandler);
}

TEST_CASE("UserTerminalRouter getSocketHandler returns handler",
          "[UserTerminalRouter]") {
  auto socketHandler = std::make_shared<PipeSocketHandler>();

  RouterEndpoint testEndpoint;

  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(testEndpoint.path);

  UserTerminalRouter router(socketHandler, routerEndpoint);

  REQUIRE(router.getSocketHandler() == socketHandler);

  testEndpoint.cleanup(socketHandler);
}
