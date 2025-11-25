#include "ClientConnection.hpp"
#include "ServerConnection.hpp"
#include "ServerClientConnection.hpp"
#include "TestHeaders.hpp"

#include <queue>

namespace et {
namespace {
// Minimal socket handler that works with socketpairs for handshake tests.
class SocketPairHandler : public SocketHandler {
 public:
  void queueConnectFd(int fd) { connectQueue.push(fd); }

  bool hasData(int fd) override { return waitOnSocketData(fd); }

  ssize_t read(int fd, void* buf, size_t count) override {
    return ::read(fd, buf, count);
  }

  ssize_t write(int fd, const void* buf, size_t count) override {
    return ::write(fd, buf, count);
  }

  int connect(const SocketEndpoint&) override {
    if (connectQueue.empty()) {
      return -1;
    }
    int fd = connectQueue.front();
    connectQueue.pop();
    return fd;
  }

  set<int> listen(const SocketEndpoint&) override { return {}; }
  set<int> getEndpointFds(const SocketEndpoint&) override { return {}; }
  int accept(int fd) override { return fd; }
  void stopListening(const SocketEndpoint&) override {}
  void close(int fd) override { ::close(fd); }
  vector<int> getActiveSockets() override { return {}; }

 private:
  std::queue<int> connectQueue;
};

class RecordingServerConnection : public ServerConnection {
 public:
  RecordingServerConnection(std::shared_ptr<SocketHandler> socketHandler,
                            const SocketEndpoint& endpoint)
      : ServerConnection(std::move(socketHandler), endpoint) {}

  bool newClient(shared_ptr<ServerClientConnection> serverClientState) override {
    newClientCalled = true;
    lastConnection = std::move(serverClientState);
    return allowNewClients;
  }

  bool newClientCalled = false;
  bool allowNewClients = true;
  shared_ptr<ServerClientConnection> lastConnection;
};

class RecoverableConnection : public Connection {
 public:
  RecoverableConnection(shared_ptr<SocketHandler> sh,
                        shared_ptr<BackedReader> r,
                        shared_ptr<BackedWriter> w, int fd, const string& key)
      : Connection(std::move(sh), "recoverable", key) {
    reader = std::move(r);
    writer = std::move(w);
    socketFd = fd;
  }

  bool recoverPublic(int fd) { return recover(fd); }

  void closeSocketAndMaybeReconnect() override { closeSocket(); }
};
}  // namespace
}  // namespace et

using namespace et;

TEST_CASE("ClientConnection completes handshake over socketpair",
          "[ClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  int fds[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  handler->queueConnectFd(fds[0]);

  const string key = "12345678901234567890123456789012";
  ClientConnection conn(handler, SocketEndpoint(), "client-id", key);

  std::thread server([&]() {
    auto request = handler->readProto<ConnectRequest>(fds[1], true);
    REQUIRE(request.clientid() == "client-id");
    REQUIRE(request.version() == PROTOCOL_VERSION);

    ConnectResponse response;
    response.set_status(RETURNING_CLIENT);
    handler->writeProto(fds[1], response, true);
  });

  REQUIRE(conn.connect());

  server.join();
  conn.shutdown();
  handler->close(fds[1]);
}

TEST_CASE("ClientConnection surfaces handshake failures", "[ClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  int fds[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  handler->queueConnectFd(fds[0]);

  const string key = "abcdefghijklmnopqrstuvwxzy123456";
  ClientConnection conn(handler, SocketEndpoint(), "untrusted-client", key);

  std::thread server([&]() {
    handler->readProto<ConnectRequest>(fds[1], true);
    ConnectResponse response;
    response.set_status(INVALID_KEY);
    response.set_error("reject");
    handler->writeProto(fds[1], response, true);
  });

  REQUIRE_FALSE(conn.connect());

  server.join();
  conn.shutdown();
  handler->close(fds[0]);
  handler->close(fds[1]);
}

TEST_CASE("ServerConnection responds to known and unknown clients",
          "[ServerConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  SocketEndpoint endpoint;
  endpoint.set_name("server");
  endpoint.set_port(0);
  RecordingServerConnection server(handler, endpoint);

  // Missing key path should return INVALID_KEY.
  int firstPair[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, firstPair) == 0);
  ConnectRequest missingKeyRequest;
  missingKeyRequest.set_clientid("missing");
  missingKeyRequest.set_version(PROTOCOL_VERSION);
  handler->writeProto(firstPair[0], missingKeyRequest, true);
  server.clientHandler(firstPair[1]);
  auto missingKeyResponse =
      handler->readProto<ConnectResponse>(firstPair[0], true);
  REQUIRE(missingKeyResponse.status() == INVALID_KEY);
  handler->close(firstPair[0]);
  handler->close(firstPair[1]);

  // Known key path should trigger newClient callback and NEW_CLIENT status.
  const string clientKey = "0123456789abcdef0123456789abcdef";
  server.addClientKey("client-one", clientKey);
  int secondPair[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, secondPair) == 0);

  std::thread serverThread([&]() { server.clientHandler(secondPair[1]); });

  ConnectRequest knownClientRequest;
  knownClientRequest.set_clientid("client-one");
  knownClientRequest.set_version(PROTOCOL_VERSION);
  handler->writeProto(secondPair[0], knownClientRequest, true);

  auto knownClientResponse =
      handler->readProto<ConnectResponse>(secondPair[0], true);
  REQUIRE(knownClientResponse.status() == NEW_CLIENT);

  serverThread.join();
  REQUIRE(server.newClientCalled);
  REQUIRE(server.clientConnectionExists("client-one"));

  handler->close(secondPair[0]);
  handler->close(secondPair[1]);
  server.shutdown();
}

TEST_CASE("ServerClientConnection verifies passkeys",
          "[ServerClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  int fds[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  const string key = "zzzyyyxxxwwwvvvuuutttsssrrrqqqpp";
  ServerClientConnection connection(handler, "client-passkey", fds[0], key);

  REQUIRE(connection.verifyPasskey(key));
  REQUIRE_FALSE(connection.verifyPasskey("zzzyyyxxxwwwvvvuuutttsssrrrqqqp"));

  connection.shutdown();
  handler->close(fds[0]);
  handler->close(fds[1]);
}

TEST_CASE("Connection recover exchanges sequence and catchup",
          "[Connection]") {
  auto handler = make_shared<SocketPairHandler>();
  int live[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, live) == 0);

  const string key = "zyxwvutsrqponmlkjihgfedcba987654";
  auto encryptCrypto = make_shared<CryptoHandler>(key, 0);
  auto decryptCrypto = make_shared<CryptoHandler>(key, 0);

  auto reader = make_shared<BackedReader>(handler, decryptCrypto, live[0]);
  auto writer = make_shared<BackedWriter>(handler, encryptCrypto, live[0]);
  RecoverableConnection conn(handler, reader, writer, live[0], key);

  conn.write(Packet(1, "first"));
  conn.write(Packet(2, "second"));
  conn.closeSocket();

  int reconnect[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, reconnect) == 0);

  std::thread remote([&]() {
    auto seqHeader = handler->readProto<SequenceHeader>(reconnect[1], true);
    REQUIRE(seqHeader.sequencenumber() == 0);

    SequenceHeader seqResponse;
    seqResponse.set_sequencenumber(1);
    handler->writeProto(reconnect[1], seqResponse, true);

    auto catchup = handler->readProto<CatchupBuffer>(reconnect[1], true);
    REQUIRE(catchup.buffer_size() == 1);
    CatchupBuffer back;
    handler->writeProto(reconnect[1], back, true);
    handler->close(reconnect[1]);
  });

  REQUIRE(conn.recoverPublic(reconnect[0]));

  conn.shutdown();
  handler->close(live[1]);
  handler->close(reconnect[0]);
  remote.join();
}
