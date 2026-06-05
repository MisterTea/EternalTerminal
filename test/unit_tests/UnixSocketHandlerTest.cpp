#include "PipeSocketHandler.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("AcceptDoesNotAbortWhenNoPendingConnection", "[UnixSocketHandler]") {
  // Regression test for the etserver crash on FreeBSD: accept() on a
  // non-blocking listening socket with no pending connection fails with
  // EAGAIN/EWOULDBLOCK.  Previously every errno other than those two hit
  // FATAL_FAIL and aborted the whole server (the same path ECONNABORTED took
  // on FreeBSD).  accept() must instead return -1 to its caller.
  shared_ptr<PipeSocketHandler> socketHandler(new PipeSocketHandler());

  string tmpPath = GetTempDirectory() + string("et_test_XXXXXXXX");
  string pipeDirectory = string(mkdtemp(&tmpPath[0]));
  string pipePath = pipeDirectory + "/pipe";

  SocketEndpoint endpoint;
  endpoint.set_name(pipePath);

  set<int> serverFds = socketHandler->listen(endpoint);
  REQUIRE(!serverFds.empty());
  int serverFd = *serverFds.begin();

  // No client has connected and the listening socket is non-blocking, so
  // accept() returns -1 rather than aborting the process.
  int clientFd = socketHandler->accept(serverFd);
  REQUIRE(clientFd == -1);
  REQUIRE((GetErrno() == EAGAIN || GetErrno() == EWOULDBLOCK));

  socketHandler->stopListening(endpoint);
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}
