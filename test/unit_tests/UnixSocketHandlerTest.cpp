#include "PipeSocketHandler.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("AcceptTransientErrorClassification", "[UnixSocketHandler]") {
  // The errnos that must be tolerated rather than aborting the server.
  // ECONNABORTED is the case that crashed etserver on FreeBSD.
  REQUIRE(UnixSocketHandler::isTransientAcceptError(EAGAIN));
  REQUIRE(UnixSocketHandler::isTransientAcceptError(EWOULDBLOCK));
  REQUIRE(UnixSocketHandler::isTransientAcceptError(ECONNABORTED));
  REQUIRE(UnixSocketHandler::isTransientAcceptError(EINTR));

  // Genuine logic errors must still be treated as fatal.
  REQUIRE_FALSE(UnixSocketHandler::isTransientAcceptError(EBADF));
  REQUIRE_FALSE(UnixSocketHandler::isTransientAcceptError(EINVAL));
  REQUIRE_FALSE(UnixSocketHandler::isTransientAcceptError(ENOTSOCK));
  REQUIRE_FALSE(UnixSocketHandler::isTransientAcceptError(EFAULT));
}

TEST_CASE("AcceptDoesNotAbortWhenNoPendingConnection", "[UnixSocketHandler]") {
  // End-to-end check: accept() on a non-blocking listening socket with no
  // pending connection fails with EAGAIN/EWOULDBLOCK and must return -1 to the
  // caller instead of hitting FATAL_FAIL.
  shared_ptr<PipeSocketHandler> socketHandler(new PipeSocketHandler());

  string tmpPath = GetTempDirectory() + string("et_test_XXXXXXXX");
  string pipeDirectory = string(mkdtemp(&tmpPath[0]));
  string pipePath = pipeDirectory + "/pipe";

  SocketEndpoint endpoint;
  endpoint.set_name(pipePath);

  set<int> serverFds = socketHandler->listen(endpoint);
  REQUIRE(!serverFds.empty());
  int serverFd = *serverFds.begin();

  int clientFd = socketHandler->accept(serverFd);
  REQUIRE(clientFd == -1);
  REQUIRE((GetErrno() == EAGAIN || GetErrno() == EWOULDBLOCK));

  socketHandler->stopListening(endpoint);
  // stopListening() only closes the fd; the bound socket file remains, so
  // remove it before the (now empty) directory.
  FATAL_FAIL(::remove(pipePath.c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}
