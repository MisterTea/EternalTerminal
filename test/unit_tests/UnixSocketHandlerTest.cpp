#include "PipeSocketHandler.hpp"
#include "TestHeaders.hpp"
#include "UnixSocketHandler.hpp"

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
  FATAL_FAIL(::remove(pipePath.c_str()));
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

namespace {

// `addToActiveSockets` is protected, and `write` rejects an unregistered fd
// before attempting any I/O, so the retry loop is unreachable without this.
//
// Derives from `PipeSocketHandler` rather than `UnixSocketHandler` because the
// latter is abstract: it leaves `connect`/`listen`/`getEndpointFds`/
// `stopListening` pure. That still exercises the code under test byte for byte
// -- `write` is pure in `SocketHandler`, defined only in `UnixSocketHandler`,
// and overridden by neither `PipeSocketHandler` nor `TcpSocketHandler`.
class TestableUnixSocketHandler : public PipeSocketHandler {
 public:
  using UnixSocketHandler::addToActiveSockets;
};

// A connected, non-blocking AF_UNIX pair. Preferred over loopback TCP because
// the kernel does not autotune AF_UNIX buffers, so the fill below is
// reproducible rather than dependent on the host's TCP memory settings.
struct WriteTestSockets {
  int a = -1;
  int b = -1;

  WriteTestSockets() {
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    a = fds[0];
    b = fds[1];
    int flags = ::fcntl(a, F_GETFL, 0);
    REQUIRE(flags != -1);
    REQUIRE(::fcntl(a, F_SETFL, flags | O_NONBLOCK) == 0);
  }
  ~WriteTestSockets() {
    if (a >= 0) ::close(a);
    if (b >= 0) ::close(b);
  }
};

// Queue bytes until the kernel refuses more, so the next write hits EAGAIN.
size_t saturate(int fd) {
  const string filler(4096, '\0');
  size_t total = 0;
  while (true) {
#ifdef MSG_NOSIGNAL
    ssize_t w = ::send(fd, filler.data(), filler.size(), MSG_NOSIGNAL);
#else
    ssize_t w = ::write(fd, filler.data(), filler.size());
#endif
    if (w < 0) {
      REQUIRE((GetErrno() == EAGAIN || GetErrno() == EWOULDBLOCK));
      return total;
    }
    total += size_t(w);
  }
}

}  // namespace

// A write that CAN eventually complete must not be abandoned partway.
//
// The peer drains slowly enough to hold the write blocked past the five-second
// mark. Before the committed-write fix this returned -1 at ~5s with a prefix
// already delivered: the peer kept the prefix, while the caller was handed a
// -1 it could not tell apart from "nothing was sent". ET's callers absorb that
// ambiguity by discarding the connection -- BackedWriter maps -1 to
// WROTE_WITH_FAILURE and Connection reconnects and replays -- so the cost of
// the old behaviour is a spurious reconnect under ordinary backpressure, not a
// corrupted stream.
//
// Takes ~7s by construction: holding the socket blocked across the old
// five-second deadline is the entire point.
TEST_CASE("WriteCompletesOnSlowlyDrainingSocket", "[UnixSocketHandler][slow]") {
  WriteTestSockets sock;
  TestableUnixSocketHandler handler;
  handler.addToActiveSockets(sock.a);

  REQUIRE(saturate(sock.a) > 0);

  std::atomic<bool> draining(true);
  std::thread reader([&]() {
    vector<char> buf(8192);
    while (draining.load()) {
      ::recv(sock.b, buf.data(), buf.size(), 0);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });

  // Larger than any plausible socket buffer, so the first send is a partial
  // write and the remainder has to wait on the drain.
  const string payload(512 * 1024, 'x');
  const auto start = std::chrono::steady_clock::now();
  ssize_t rc = handler.write(sock.a, payload.data(), payload.size());
  const auto elapsed = std::chrono::steady_clock::now() - start;

  draining = false;
  reader.join();

  REQUIRE(rc == ssize_t(payload.size()));
  // Finishing quickly would mean the drain outran the old deadline, making the
  // test green without exercising the regression at all.
  REQUIRE(elapsed > std::chrono::seconds(5));
}

// The opposite guarantee: the five-second budget still applies while NOTHING is
// committed. Giving up there is clean -- the peer has seen none of the message,
// so -1 is the whole truth -- and removing it would turn a dead socket into a
// minute-long stall on every write.
//
// Takes ~5s by construction.
TEST_CASE("WriteStillGivesUpWhenNothingWasSent", "[UnixSocketHandler][slow]") {
  WriteTestSockets sock;
  TestableUnixSocketHandler handler;
  handler.addToActiveSockets(sock.a);

  // Full, and the peer never reads, so not one byte of the payload can land.
  REQUIRE(saturate(sock.a) > 0);

  const string payload(1024, 'y');
  const auto start = std::chrono::steady_clock::now();
  ssize_t rc = handler.write(sock.a, payload.data(), payload.size());
  const auto elapsed = std::chrono::steady_clock::now() - start;

  REQUIRE(rc == -1);
  REQUIRE(elapsed >= std::chrono::seconds(5));
  // Must not have escalated to the committed budget: nothing reached the wire.
  REQUIRE(elapsed < std::chrono::seconds(30));
}

// A closed peer is a real error rather than backpressure, and must fail
// immediately instead of spinning out either budget.
TEST_CASE("WriteFailsFastOnClosedPeer", "[UnixSocketHandler]") {
  WriteTestSockets sock;
  TestableUnixSocketHandler handler;
  handler.addToActiveSockets(sock.a);

  ::close(sock.b);
  sock.b = -1;

  const string payload(1024, 'z');
  const auto start = std::chrono::steady_clock::now();
  ssize_t rc = handler.write(sock.a, payload.data(), payload.size());
  const auto elapsed = std::chrono::steady_clock::now() - start;

  REQUIRE(rc == -1);
  REQUIRE(elapsed < std::chrono::seconds(2));
}
#endif
