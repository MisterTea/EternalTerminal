/**
 * Regression test for accept starvation while a client reconnects.
 *
 * A returning client whose network path died blocks inside
 * Connection::recover() until the socket timeout expires. The server must keep
 * accepting new connections while that happens, so the reconnect path must not
 * hold a server-wide lock across blocking socket I/O.
 */

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <future>
#include <thread>

#include "ServerClientConnection.hpp"
#include "ServerConnection.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {
// Socket handler driven by pre-made socketpairs. accept() hands back the
// descriptor it is given so acceptNewConnection() can be called directly.
class SocketPairHandler : public SocketHandler {
 public:
  bool hasData(int fd) override { return waitOnSocketData(fd); }

  ssize_t read(int fd, void* buf, size_t count) override {
    return ::read(fd, buf, count);
  }

  ssize_t write(int fd, const void* buf, size_t count) override {
    return ::write(fd, buf, count);
  }

  int connect(const SocketEndpoint&) override { return -1; }
  set<int> listen(const SocketEndpoint&) override { return {}; }
  set<int> getEndpointFds(const SocketEndpoint&) override { return {}; }
  int accept(int fd) override { return fd; }
  void stopListening(const SocketEndpoint&) override {}
  void close(int fd) override { ::close(fd); }
  vector<int> getActiveSockets() override { return {}; }
};

class TestServerConnection : public ServerConnection {
 public:
  TestServerConnection(std::shared_ptr<SocketHandler> socketHandler,
                       const SocketEndpoint& endpoint)
      : ServerConnection(std::move(socketHandler), endpoint) {}

  // The real server starts a session thread here; the test only needs the
  // connection to stay registered.
  bool newClient(shared_ptr<ServerClientConnection>) override { return true; }
};

void writeConnectRequest(const shared_ptr<SocketHandler>& handler, int fd,
                         const string& clientId) {
  ConnectRequest request;
  request.set_clientid(clientId);
  request.set_version(PROTOCOL_VERSION);
  handler->writeProto(fd, request, true);
}
}  // namespace

TEST_CASE("Reconnect stuck in recover still allows new connections",
          "[AcceptStarvation][ServerConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  SocketEndpoint endpoint;
  endpoint.set_name("server");
  TestServerConnection server(handler, endpoint);

  const string clientId = "starved-client";
  server.addClientKey(clientId, "abcdefghijklmnopqrstuvwxyz012345");

  int live[2] = {-1, -1};
  int stuck[2] = {-1, -1};
  int fresh[2] = {-1, -1};
  std::thread reconnectThread;
  std::future<bool> accepted;

  // Runs before the thread/future are destroyed: closing the stuck peer lets
  // recover() fail and release whatever it holds, so nothing hangs on teardown.
  struct Cleanup {
    TestServerConnection& server;
    std::thread& reconnectThread;
    int& stuckPeerFd;
    int& livePeerFd;
    int& freshPeerFd;
    ~Cleanup() {
      if (stuckPeerFd >= 0) {
        ::close(stuckPeerFd);
        stuckPeerFd = -1;
      }
      if (reconnectThread.joinable()) {
        reconnectThread.join();
      }
      if (livePeerFd >= 0) {
        ::close(livePeerFd);
        livePeerFd = -1;
      }
      if (freshPeerFd >= 0) {
        ::close(freshPeerFd);
        freshPeerFd = -1;
      }
      server.shutdown();
    }
  } cleanup{server, reconnectThread, stuck[1], live[1], fresh[1]};

  // A live session the reconnect can come back to.
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, live) == 0);
  writeConnectRequest(handler, live[1], clientId);
  server.clientHandler(live[0]);
  REQUIRE(server.clientConnectionExists(clientId));
  handler->readProto<ConnectResponse>(live[1], true);

  // The reconnect. Nothing ever answers on the peer end, so recover() blocks
  // waiting for the sequence header reply.
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, stuck) == 0);
  writeConnectRequest(handler, stuck[1], clientId);
  reconnectThread = std::thread([&]() { server.clientHandler(stuck[0]); });

  // recover() is provably in flight once it has answered RETURNING_CLIENT and
  // written its sequence header; the next thing it does is block on the reply.
  handler->readProto<ConnectResponse>(stuck[1], true);
  handler->readProto<SequenceHeader>(stuck[1], true,
                                     SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);

  // An unrelated client must still get accepted. The pooled handler owns and
  // closes fresh[0]; an oversized length makes it fail fast instead of
  // occupying a worker.
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fresh) == 0);
  int64_t oversize = SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH + 1;
  REQUIRE(handler->writeAllOrReturn(fresh[1], &oversize, sizeof(oversize)) ==
          (int)sizeof(oversize));

  accepted = std::async(std::launch::async,
                        [&]() { return server.acceptNewConnection(fresh[0]); });

  INFO("acceptNewConnection blocked while another client was recovering");
  REQUIRE(accepted.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
  REQUIRE(accepted.get());
}
