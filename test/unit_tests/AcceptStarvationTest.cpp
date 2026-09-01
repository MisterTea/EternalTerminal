/**
 * Regression test for accept starvation while a client reconnects.
 *
 * A returning client whose network path died blocks inside
 * Connection::recover() until the socket timeout expires. The server must keep
 * accepting new connections while that happens, so the reconnect path must not
 * hold a server-wide lock across blocking socket I/O.
 */

#include <chrono>
#include <future>
#include <thread>

#include "ServerClientConnection.hpp"
#include "ServerConnection.hpp"
#include "TestHeaders.hpp"
#include "TestSocketPair.hpp"

using namespace et;
using namespace et::test;

namespace {
// Socket handler driven by pre-made socketpairs. accept() hands back the
// descriptor it is given so acceptNewConnection() can be called directly.
class SocketPairHandler : public SocketHandler {
 public:
  bool hasData(int fd) override { return waitOnSocketData(fd); }

  ssize_t read(int fd, void* buf, size_t count) override {
#ifdef WIN32
    return ::recv(fd, static_cast<char*>(buf), static_cast<int>(count), 0);
#else
    return ::read(fd, buf, count);
#endif
  }

  ssize_t write(int fd, const void* buf, size_t count) override {
#ifdef WIN32
    return ::send(fd, static_cast<const char*>(buf), static_cast<int>(count),
                  0);
#else
    return ::write(fd, buf, count);
#endif
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

// Runs before the thread/future are destroyed: closing the stuck peer lets
// recover() fail and release whatever it holds, so nothing hangs on teardown.
struct Cleanup {
  ServerConnection& server;
  std::thread& reconnectThread;
  int& stuckPeerFd;
  int& livePeerFd;
  int& extraPeerFd;
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
    if (extraPeerFd >= 0) {
      ::close(extraPeerFd);
      extraPeerFd = -1;
    }
    server.shutdown();
  }
};

// Leaves the server in the state the incident produces: a registered session
// plus a reconnect for it blocked inside recover(). Returns once that block is
// provable, so a test can act while it is still in effect.
void wedgeReconnect(const shared_ptr<SocketHandler>& handler,
                    TestServerConnection& server, const string& clientId,
                    int live[2], int stuck[2], std::thread& reconnectThread) {
  // A live session the reconnect can come back to.
  REQUIRE(createTestSocketPair(live) == 0);
  writeConnectRequest(handler, live[1], clientId);
  server.clientHandler(live[0]);
  REQUIRE(server.clientConnectionExists(clientId));
  handler->readProto<ConnectResponse>(live[1], true);

  // The reconnect. Nothing ever answers on the peer end, so recover() blocks
  // waiting for the sequence header reply.
  REQUIRE(createTestSocketPair(stuck) == 0);
  writeConnectRequest(handler, stuck[1], clientId);
  const int stuckServerFd = stuck[0];
  reconnectThread = std::thread(
      [&server, stuckServerFd]() { server.clientHandler(stuckServerFd); });

  // recover() is provably in flight once it has answered RETURNING_CLIENT and
  // written its sequence header; the next thing it does is block on the reply.
  handler->readProto<ConnectResponse>(stuck[1], true);
  handler->readProto<SequenceHeader>(stuck[1], true,
                                     SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
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
  Cleanup cleanup{server, reconnectThread, stuck[1], live[1], fresh[1]};

  wedgeReconnect(handler, server, clientId, live, stuck, reconnectThread);

  // An unrelated client must still get accepted. The pooled handler owns and
  // closes fresh[0]; an oversized length makes it fail fast instead of
  // occupying a worker.
  REQUIRE(createTestSocketPair(fresh) == 0);
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

TEST_CASE("A second reconnect is refused while one is in flight",
          "[AcceptStarvation][ServerClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  SocketEndpoint endpoint;
  endpoint.set_name("server");
  TestServerConnection server(handler, endpoint);

  const string clientId = "starved-client";
  server.addClientKey(clientId, "abcdefghijklmnopqrstuvwxyz012345");

  int live[2] = {-1, -1};
  int stuck[2] = {-1, -1};
  int second[2] = {-1, -1};
  std::thread reconnectThread;
  std::future<void> refused;
  Cleanup cleanup{server, reconnectThread, stuck[1], live[1], second[1]};

  wedgeReconnect(handler, server, clientId, live, stuck, reconnectThread);

  // A second reconnect for the same client, arriving while the first is still
  // blocked. Queueing it would hold this handler until the first one's socket
  // timeout expires, and handlers are the resource every other client needs.
  REQUIRE(createTestSocketPair(second) == 0);
  writeConnectRequest(handler, second[1], clientId);
  refused = std::async(std::launch::async,
                       [&]() { server.clientHandler(second[0]); });

  INFO("the second reconnect waited for the first instead of being refused");
  REQUIRE(refused.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
  refused.get();

  // It is answered and then dropped, so the client retries on its own schedule.
  REQUIRE(handler->readProto<ConnectResponse>(second[1], true).status() ==
          RETURNING_CLIENT);
  REQUIRE_THROWS(handler->readProto<SequenceHeader>(
      second[1], true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH));
  // The live session is untouched: only the refused socket was closed.
  REQUIRE(server.clientConnectionExists(clientId));
}
