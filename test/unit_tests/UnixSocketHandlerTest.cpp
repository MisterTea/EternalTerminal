#include "PipeSocketHandler.hpp"
#include "TestHeaders.hpp"

using namespace et;

#ifndef WIN32
namespace {
class TestPipeSocketHandler : public PipeSocketHandler {
 public:
  void trackSocket(int fd) {
    addToActiveSockets(fd);
    initSocket(fd);
  }
};
}  // namespace

TEST_CASE("WriteReturnsCommittedPrefixOnTimeout", "[UnixSocketHandler]") {
  int sockets[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

  int sendBufferSize = 4096;
  REQUIRE(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &sendBufferSize,
                       sizeof(sendBufferSize)) == 0);

  TestPipeSocketHandler socketHandler;
  socketHandler.trackSocket(sockets[0]);

  const string filler(4096, 'f');
  size_t fillerBytes = 0;
  while (true) {
    const ssize_t result = ::send(sockets[0], filler.data(), filler.size(), 0);
    if (result > 0) {
      fillerBytes += result;
      continue;
    }
    REQUIRE(result == -1);
    REQUIRE((GetErrno() == EAGAIN || GetErrno() == EWOULDBLOCK));
    break;
  }

  array<char, 4096> receiveBuffer;
  size_t drainedFillerBytes = 0;
  pollfd writable{.fd = sockets[0], .events = POLLOUT, .revents = 0};
  do {
    const ssize_t result =
        ::recv(sockets[1], receiveBuffer.data(), receiveBuffer.size(), 0);
    REQUIRE(result > 0);
    drainedFillerBytes += result;
    writable.revents = 0;
    REQUIRE(::poll(&writable, 1, 0) >= 0);
  } while ((writable.revents & POLLOUT) == 0);

  const string payload(fillerBytes * 2, 'p');
  const ssize_t writeResult =
      socketHandler.write(sockets[0], payload.data(), payload.size());

  const int peerFlags = ::fcntl(sockets[1], F_GETFL);
  REQUIRE(peerFlags >= 0);
  REQUIRE(::fcntl(sockets[1], F_SETFL, peerFlags | O_NONBLOCK) == 0);

  vector<char> queuedBytes;
  while (true) {
    const ssize_t result =
        ::recv(sockets[1], receiveBuffer.data(), receiveBuffer.size(), 0);
    if (result > 0) {
      queuedBytes.insert(queuedBytes.end(), receiveBuffer.begin(),
                         receiveBuffer.begin() + result);
      continue;
    }
    REQUIRE(result == -1);
    REQUIRE((GetErrno() == EAGAIN || GetErrno() == EWOULDBLOCK));
    break;
  }

  const size_t remainingFillerBytes = fillerBytes - drainedFillerBytes;
  REQUIRE(queuedBytes.size() > remainingFillerBytes);
  const size_t committedPayloadBytes =
      queuedBytes.size() - remainingFillerBytes;
  REQUIRE(std::all_of(queuedBytes.begin(),
                      queuedBytes.begin() + remainingFillerBytes,
                      [](char byte) { return byte == 'f'; }));
  REQUIRE(std::all_of(queuedBytes.begin() + remainingFillerBytes,
                      queuedBytes.end(),
                      [](char byte) { return byte == 'p'; }));

  socketHandler.close(sockets[0]);
  REQUIRE(::close(sockets[1]) == 0);

  REQUIRE(committedPayloadBytes > 0);
  REQUIRE(committedPayloadBytes < payload.size());
  REQUIRE(writeResult == static_cast<ssize_t>(committedPayloadBytes));
}

TEST_CASE("WriteReturnsCommittedPrefixOnHardError", "[UnixSocketHandler]") {
  int sockets[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

  int sendBufferSize = 4096;
  REQUIRE(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &sendBufferSize,
                       sizeof(sendBufferSize)) == 0);

  TestPipeSocketHandler socketHandler;
  socketHandler.trackSocket(sockets[0]);

  const string payload(1024 * 1024, 'p');
  ssize_t writeResult = 0;
  int writeErrno = 0;
  thread writer([&]() {
    writeResult =
        socketHandler.write(sockets[0], payload.data(), payload.size());
    writeErrno = GetErrno();
  });

  array<char, 4096> receiveBuffer;
  vector<char> receivedBytes;
  pollfd readable{.fd = sockets[1], .events = POLLIN, .revents = 0};
  const int pollResult = ::poll(&readable, 1, 5000);
  ssize_t firstRead = -1;
  if (pollResult == 1) {
    firstRead =
        ::recv(sockets[1], receiveBuffer.data(), receiveBuffer.size(), 0);
  }
  if (firstRead > 0) {
    receivedBytes.insert(receivedBytes.end(), receiveBuffer.begin(),
                         receiveBuffer.begin() + firstRead);
  }

  const int shutdownResult = ::shutdown(sockets[0], SHUT_WR);
  writer.join();

  socketHandler.close(sockets[0]);
  ssize_t readResult;
  while ((readResult = ::recv(sockets[1], receiveBuffer.data(),
                              receiveBuffer.size(), 0)) > 0) {
    receivedBytes.insert(receivedBytes.end(), receiveBuffer.begin(),
                         receiveBuffer.begin() + readResult);
  }

  const int closeResult = ::close(sockets[1]);

  REQUIRE(pollResult == 1);
  REQUIRE(firstRead > 0);
  REQUIRE(shutdownResult == 0);
  REQUIRE(readResult == 0);
  REQUIRE(closeResult == 0);
  REQUIRE(writeErrno == EPIPE);
  REQUIRE(receivedBytes.size() < payload.size());
  REQUIRE(
      std::equal(receivedBytes.begin(), receivedBytes.end(), payload.begin()));
  REQUIRE(writeResult == static_cast<ssize_t>(receivedBytes.size()));
}
#endif

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

#ifdef WIN32
  string pipePath = "et_unix_socket_test_" + genRandomAlphaNum(12) + ".ipc";
#else
  string tmpPath = GetTempDirectory() + string("et_test_XXXXXXXX");
  string pipeDirectory = string(mkdtemp(&tmpPath[0]));
  string pipePath = pipeDirectory + "/pipe";
#endif

  SocketEndpoint endpoint;
  endpoint.set_name(pipePath);

  set<int> serverFds = socketHandler->listen(endpoint);
  REQUIRE(!serverFds.empty());
  int serverFd = *serverFds.begin();

  int clientFd = socketHandler->accept(serverFd);
  REQUIRE(clientFd == -1);
  REQUIRE((GetErrno() == EAGAIN || GetErrno() == EWOULDBLOCK));

  socketHandler->stopListening(endpoint);
#ifdef WIN32
  REQUIRE_FALSE(fs::exists(pipePath));
#else
  REQUIRE(::access(pipePath.c_str(), F_OK) != 0);
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
#endif
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
