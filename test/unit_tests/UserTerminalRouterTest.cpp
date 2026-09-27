#include <chrono>

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

#ifndef WIN32
namespace {
class CountingPipeSocketHandler : public PipeSocketHandler {
 public:
  void close(int fd) override {
    {
      lock_guard<mutex> guard(closeCountsMutex);
      closeCounts[fd]++;
    }
    PipeSocketHandler::close(fd);
  }

  int closeCount(int fd) const {
    lock_guard<mutex> guard(closeCountsMutex);
    const auto it = closeCounts.find(fd);
    return it == closeCounts.end() ? 0 : it->second;
  }

 private:
  mutable mutex closeCountsMutex;
  map<int, int> closeCounts;
};

class InspectableUserTerminalRouter : public UserTerminalRouter {
 public:
  using UserTerminalRouter::UserTerminalRouter;

  int terminalFd(const string& id) {
    lock_guard<recursive_mutex> guard(routerMutex);
    return idInfoMap.at(id).fd();
  }
};

int registerFakeTerminal(shared_ptr<PipeSocketHandler> socketHandler,
                         UserTerminalRouter& router,
                         const SocketEndpoint& routerEndpoint, const string& id,
                         const string& passkey, bool ptyActive,
                         IdKeyPair* accepted) {
  int terminalFd = socketHandler->connect(routerEndpoint);
  REQUIRE(terminalFd >= 0);
  TerminalUserInfo tui;
  tui.set_id(id);
  tui.set_passkey(passkey);
  tui.set_uid(getuid());
  tui.set_gid(getgid());
  tui.set_ptyactive(ptyActive);
  socketHandler->writePacket(
      terminalFd,
      Packet(TerminalPacketType::TERMINAL_USER_INFO, protoToString(tui)));
  *accepted = router.acceptNewConnection();
  return terminalFd;
}
}  // namespace

TEST_CASE("UserTerminalRouter tracks ptyactive registrations",
          "[UserTerminalRouter]") {
  auto socketHandler = std::make_shared<PipeSocketHandler>();

  RouterEndpoint testEndpoint;

  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(testEndpoint.path);

  UserTerminalRouter router(socketHandler, routerEndpoint);
  REQUIRE_FALSE(router.isPtyActive("missing"));
  auto missingConnection = make_shared<ServerClientConnection>(
      socketHandler, "missing", -1, "0123456789abcdef0123456789abcdef");
  REQUIRE_FALSE(router.tryGetInfoForConnection(missingConnection));

  IdKeyPair accepted;
  int fdA = registerFakeTerminal(socketHandler, router, routerEndpoint,
                                 "term-a", "key-a", false, &accepted);
  REQUIRE(accepted.id == "term-a");
  REQUIRE(accepted.key == "key-a");
  REQUIRE_FALSE(router.isPtyActive("term-a"));

  int fdB = registerFakeTerminal(socketHandler, router, routerEndpoint,
                                 "term-b", "key-b", true, &accepted);
  REQUIRE(accepted.id == "term-b");
  REQUIRE(router.isPtyActive("term-b"));

  socketHandler->close(fdA);
  socketHandler->close(fdB);
  testEndpoint.cleanup(socketHandler);
}

TEST_CASE("UserTerminalRouter replaces dead registrations only",
          "[UserTerminalRouter]") {
  auto socketHandler = std::make_shared<CountingPipeSocketHandler>();

  RouterEndpoint testEndpoint;

  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(testEndpoint.path);

  InspectableUserTerminalRouter router(socketHandler, routerEndpoint);

  IdKeyPair accepted;
  int fdOwner = registerFakeTerminal(socketHandler, router, routerEndpoint,
                                     "term", "owner-key", true, &accepted);
  REQUIRE(accepted.id == "term");
  REQUIRE(accepted.key == "owner-key");
  const int supersededRouterFd = router.terminalFd("term");

  int fdDup = registerFakeTerminal(socketHandler, router, routerEndpoint,
                                   "term", "attacker-key", true, &accepted);
  REQUIRE(accepted.id == "");
  REQUIRE(router.isPtyActive("term"));
  socketHandler->close(fdDup);

  int fdSameKey = registerFakeTerminal(socketHandler, router, routerEndpoint,
                                       "term", "owner-key", true, &accepted);
  REQUIRE(accepted.id.empty());
  REQUIRE(router.terminalFd("term") == supersededRouterFd);
  socketHandler->close(fdSameKey);

  socketHandler->close(fdOwner);
  int fdWrongKey =
      registerFakeTerminal(socketHandler, router, routerEndpoint, "term",
                           "attacker-key", true, &accepted);
  CHECK(accepted.id.empty());
  CHECK(router.terminalFd("term") == supersededRouterFd);
  CHECK(socketHandler->closeCount(supersededRouterFd) == 0);
  socketHandler->close(fdWrongKey);
  int fdNew = registerFakeTerminal(socketHandler, router, routerEndpoint,
                                   "term", "owner-key", true, &accepted);
  REQUIRE(accepted.id == "term");
  REQUIRE(accepted.key == "owner-key");
  REQUIRE(router.isPtyActive("term"));
  REQUIRE(socketHandler->closeCount(supersededRouterFd) == 1);

  socketHandler->close(fdNew);
  testEndpoint.cleanup(socketHandler);
}

TEST_CASE("UserTerminalRouter removeTerminal frees the id",
          "[UserTerminalRouter]") {
  auto socketHandler = std::make_shared<CountingPipeSocketHandler>();

  RouterEndpoint testEndpoint;

  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(testEndpoint.path);

  InspectableUserTerminalRouter router(socketHandler, routerEndpoint);

  IdKeyPair accepted;
  int fdOwner = registerFakeTerminal(socketHandler, router, routerEndpoint,
                                     "term", "owner-key", true, &accepted);
  REQUIRE(accepted.id == "term");

  const int routerFd = router.terminalFd("term");
  const UserTerminalRouter& constRouter = router;
  REQUIRE(constRouter.isCurrentRegistration("term", routerFd));
  REQUIRE_FALSE(constRouter.isCurrentRegistration("term", routerFd + 1));
  const auto removalStarted = std::chrono::steady_clock::now();
  REQUIRE(router.removeTerminal("term", routerFd));
  const auto removalElapsed = std::chrono::steady_clock::now() - removalStarted;
  REQUIRE(removalElapsed < std::chrono::milliseconds(500));
  REQUIRE_FALSE(constRouter.isCurrentRegistration("term", routerFd));
  REQUIRE_FALSE(router.isPtyActive("term"));
  REQUIRE(socketHandler->closeCount(routerFd) == 1);

  int fdNew = registerFakeTerminal(socketHandler, router, routerEndpoint,
                                   "term", "owner-key", true, &accepted);
  REQUIRE(accepted.id == "term");

  socketHandler->close(fdOwner);
  socketHandler->close(fdNew);
  testEndpoint.cleanup(socketHandler);
}
#endif
