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

namespace {
// Registers a fake terminal with the router over the pipe endpoint, exactly
// like UserTerminalHandler::registerWithRouter does.  Returns the terminal
// side fd and the accepted id/key pair via the out parameters.
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

  string tmpPath =
      GetTempDirectory() + string("et_test_router_ptyactive_XXXXXXXX");
  string pipeDirectory = string(mkdtemp(&tmpPath[0]));
  string pipePath = pipeDirectory + "/router_pipe";

  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(pipePath);

  UserTerminalRouter router(socketHandler, routerEndpoint);
  REQUIRE_FALSE(router.isPtyActive("missing"));

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
  socketHandler->close(router.getServerFd());
  FATAL_FAIL(::remove(pipePath.c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

TEST_CASE("UserTerminalRouter replaces dead registrations only",
          "[UserTerminalRouter]") {
  auto socketHandler = std::make_shared<PipeSocketHandler>();

  string tmpPath =
      GetTempDirectory() + string("et_test_router_replace_XXXXXXXX");
  string pipeDirectory = string(mkdtemp(&tmpPath[0]));
  string pipePath = pipeDirectory + "/router_pipe";

  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(pipePath);

  UserTerminalRouter router(socketHandler, routerEndpoint);

  IdKeyPair accepted;
  int fdOwner = registerFakeTerminal(socketHandler, router, routerEndpoint,
                                     "term", "owner-key", true, &accepted);
  REQUIRE(accepted.id == "term");
  REQUIRE(accepted.key == "owner-key");

  // A duplicate registration while the owner is live is rejected, and the
  // original registration (and its key) stays in place.
  int fdDup = registerFakeTerminal(socketHandler, router, routerEndpoint,
                                   "term", "attacker-key", true, &accepted);
  REQUIRE(accepted.id == "");
  REQUIRE(router.isPtyActive("term"));
  // The router closed the rejected duplicate's fd.
  socketHandler->close(fdDup);

  // Once the owner's pipe dies, the same id can register again (this is the
  // re-attach path after the connection dropped without the session ending).
  socketHandler->close(fdOwner);
  int fdNew = registerFakeTerminal(socketHandler, router, routerEndpoint,
                                   "term", "owner-key", true, &accepted);
  REQUIRE(accepted.id == "term");
  REQUIRE(accepted.key == "owner-key");
  REQUIRE(router.isPtyActive("term"));

  socketHandler->close(fdNew);
  socketHandler->close(router.getServerFd());
  FATAL_FAIL(::remove(pipePath.c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

TEST_CASE("UserTerminalRouter removeTerminal frees the id",
          "[UserTerminalRouter]") {
  auto socketHandler = std::make_shared<PipeSocketHandler>();

  string tmpPath =
      GetTempDirectory() + string("et_test_router_remove_XXXXXXXX");
  string pipeDirectory = string(mkdtemp(&tmpPath[0]));
  string pipePath = pipeDirectory + "/router_pipe";

  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(pipePath);

  UserTerminalRouter router(socketHandler, routerEndpoint);

  IdKeyPair accepted;
  int fdOwner = registerFakeTerminal(socketHandler, router, routerEndpoint,
                                     "term", "owner-key", true, &accepted);
  REQUIRE(accepted.id == "term");

  router.removeTerminal("term");
  REQUIRE_FALSE(router.isPtyActive("term"));

  // Even with the old pipe still open, the id is free again.
  int fdNew = registerFakeTerminal(socketHandler, router, routerEndpoint,
                                   "term", "owner-key", true, &accepted);
  REQUIRE(accepted.id == "term");

  socketHandler->close(fdOwner);
  socketHandler->close(fdNew);
  socketHandler->close(router.getServerFd());
  FATAL_FAIL(::remove(pipePath.c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

#endif
