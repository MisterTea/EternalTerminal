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
  REQUIRE(::access(pipePath.c_str(), F_OK) != 0);
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

#ifndef WIN32
TEST_CASE("PipeSocketHandler listenAsUser and connectAsUser",
          "[UnixSocketHandler][PipeSocketHandler]") {
  shared_ptr<PipeSocketHandler> socketHandler(new PipeSocketHandler());

  string tmpPath = GetTempDirectory() + string("et_test_user_XXXXXXXX");
  string pipeDirectory = string(mkdtemp(&tmpPath[0]));
  string pipePath = pipeDirectory + "/pipe";

  SocketEndpoint endpoint;
  endpoint.set_name(pipePath);

  uid_t uid = getuid();
  gid_t gid = getgid();
  set<int> serverFds = socketHandler->listenAsUser(endpoint, uid, gid);
  REQUIRE(!serverFds.empty());

  REQUIRE_THROWS_AS(socketHandler->listenAsUser(endpoint, uid, gid),
                    std::runtime_error);

  int clientFd = socketHandler->connectAsUser(endpoint, uid, gid);
  REQUIRE(clientFd >= 0);

  int accepted = socketHandler->accept(*serverFds.begin());
  REQUIRE(accepted >= 0);

  socketHandler->close(accepted);
  socketHandler->close(clientFd);
  socketHandler->stopListening(endpoint);
  REQUIRE(::access(pipePath.c_str(), F_OK) != 0);
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

TEST_CASE("PipeSocketHandler connectAsUser returns -1 when path missing",
          "[UnixSocketHandler][PipeSocketHandler]") {
  shared_ptr<PipeSocketHandler> socketHandler(new PipeSocketHandler());

  string tmpPath = GetTempDirectory() + string("et_test_user_XXXXXXXX");
  string pipeDirectory = string(mkdtemp(&tmpPath[0]));
  string pipePath = pipeDirectory + "/missing";

  SocketEndpoint endpoint;
  endpoint.set_name(pipePath);

  int clientFd = socketHandler->connectAsUser(endpoint, getuid(), getgid());
  REQUIRE(clientFd < 0);

  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

TEST_CASE("PipeSocketHandler listenAsUser throws when path cannot bind",
          "[UnixSocketHandler][PipeSocketHandler]") {
  if (getuid() == 0) {
    SKIP("Test requires a non-root process");
  }

  shared_ptr<PipeSocketHandler> socketHandler(new PipeSocketHandler());
  SocketEndpoint endpoint;
  endpoint.set_name("/dev/null_et_listen_as_user_should_fail");
  REQUIRE_THROWS_AS(socketHandler->listenAsUser(endpoint, getuid(), getgid()),
                    std::runtime_error);
}
#endif
