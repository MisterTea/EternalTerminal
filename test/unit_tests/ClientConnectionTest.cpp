#include <queue>

#include "ClientConnection.hpp"
#include "ServerClientConnection.hpp"
#include "ServerConnection.hpp"
#include "TestHeaders.hpp"

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

  bool newClient(
      shared_ptr<ServerClientConnection> serverClientState) override {
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
                        shared_ptr<BackedReader> r, shared_ptr<BackedWriter> w,
                        int fd, const string& key)
      : Connection(std::move(sh), "recoverable", key) {
    reader = std::move(r);
    writer = std::move(w);
    socketFd = fd;
  }

  bool recoverPublic(int fd, bool forceReset = false) {
    return recover(fd, forceReset);
  }

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

    // The returning client starts a reset recovery exchange.
    auto seqHeader = handler->readProto<SequenceHeader>(
        fds[1], true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
    REQUIRE(seqHeader.reset());
    SequenceHeader seqResponse;
    seqResponse.set_sequencenumber(0);
    seqResponse.set_reset(true);
    handler->writeProto(fds[1], seqResponse, true);
    auto catchup = handler->readProto<CatchupBuffer>(fds[1], true);
    REQUIRE(catchup.buffer_size() == 0);
    CatchupBuffer back;
    handler->writeProto(fds[1], back, true);
  });

  REQUIRE(conn.connect());
  REQUIRE(conn.wasRecovered());

  server.join();
  conn.shutdown();
  handler->close(fds[1]);
}

TEST_CASE("ClientConnection surfaces handshake failures",
          "[ClientConnection]") {
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

TEST_CASE("ServerClientConnection recoverClient keeps old socket on failure",
          "[ServerClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  int live[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, live) == 0);

  const string key = "zyxwvutsrqponmlkjihgfedcba987654";
  ServerClientConnection connection(handler, "client-recover", live[0], key);
  REQUIRE(connection.getSocketFd() == live[0]);

  int attack[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, attack) == 0);

  std::thread attacker([&]() {
    // Read server SequenceHeader, then claim to be far ahead.
    handler->readProto<SequenceHeader>(
        attack[1], true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
    SequenceHeader bad;
    bad.set_sequencenumber(999999);
    handler->writeProto(attack[1], bad, true);
  });

  REQUIRE_FALSE(connection.recoverClient(attack[0]));
  REQUIRE(connection.getSocketFd() == live[0]);

  attacker.join();
  connection.shutdown();
  handler->close(live[0]);
  handler->close(live[1]);
  // attack[0] closed inside recover on failure; attack[1] may still be open.
  handler->close(attack[1]);
}

TEST_CASE("ServerClientConnection recoverClient closes old socket on success",
          "[ServerClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  int live[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, live) == 0);

  const string key = "zyxwvutsrqponmlkjihgfedcba987654";
  ServerClientConnection connection(handler, "client-recover-ok", live[0], key);
  REQUIRE(connection.getSocketFd() == live[0]);

  int reconnect[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, reconnect) == 0);

  std::thread remote([&]() {
    auto seqHeader = handler->readProto<SequenceHeader>(
        reconnect[1], true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
    REQUIRE(seqHeader.sequencenumber() == 0);

    SequenceHeader seqResponse;
    seqResponse.set_sequencenumber(0);
    handler->writeProto(reconnect[1], seqResponse, true);

    auto catchup = handler->readProto<CatchupBuffer>(reconnect[1], true);
    REQUIRE(catchup.buffer_size() == 0);
    CatchupBuffer back;
    handler->writeProto(reconnect[1], back, true);
  });

  REQUIRE(connection.recoverClient(reconnect[0]));
  REQUIRE(connection.getSocketFd() == reconnect[0]);

  remote.join();
  connection.shutdown();
  handler->close(live[1]);
  handler->close(reconnect[0]);
  handler->close(reconnect[1]);
}

TEST_CASE("Connection recover exchanges sequence and catchup", "[Connection]") {
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

TEST_CASE("Connection recover with forceReset performs clean reset exchange",
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
    auto seqHeader = handler->readProto<SequenceHeader>(
        reconnect[1], true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
    REQUIRE(seqHeader.sequencenumber() == 0);
    REQUIRE(seqHeader.reset());

    // The remote peer is further ahead; with a reset its history is
    // discarded, so this must not trigger a "client is ahead" failure.
    SequenceHeader seqResponse;
    seqResponse.set_sequencenumber(5);
    seqResponse.set_reset(true);
    handler->writeProto(reconnect[1], seqResponse, true);

    auto catchup = handler->readProto<CatchupBuffer>(reconnect[1], true);
    REQUIRE(catchup.buffer_size() == 0);
    CatchupBuffer back;
    handler->writeProto(reconnect[1], back, true);
  });

  REQUIRE(conn.recoverPublic(reconnect[0], true));
  remote.join();

  REQUIRE(conn.getReader()->getSequenceNumber() == 0);
  REQUIRE(conn.getWriter()->getSequenceNumber() == 0);
  // A write after reset starts a fresh sequence at 1.
  conn.write(Packet(3, "after-reset"));
  REQUIRE(conn.getWriter()->getSequenceNumber() == 1);
}

TEST_CASE(
    "ClientConnection connect performs reset recovery for returning "
    "clients",
    "[ClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  const string key = "zyxwvutsrqponmlkjihgfedcba987654";

  // A server-side connection with existing history: the old client exited,
  // but the server state (and its buffered output) survives.
  int live[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, live) == 0);
  ServerClientConnection serverConn(handler, "client-id", live[0], key);
  serverConn.writePacket(Packet(1, "pre-existing-output"));

  // The old client disconnects.
  handler->close(live[1]);

  // A fresh client process connects with the same id.
  int reconnect[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, reconnect) == 0);
  handler->queueConnectFd(reconnect[0]);
  ClientConnection client(handler, SocketEndpoint(), "client-id", key);

  std::thread server([&]() {
    auto request = handler->readProto<ConnectRequest>(reconnect[1], true);
    REQUIRE(request.clientid() == "client-id");
    ConnectResponse response;
    response.set_status(RETURNING_CLIENT);
    handler->writeProto(reconnect[1], response, true);
    // The client's reset request makes the server take the reset path too.
    REQUIRE(serverConn.recoverClient(reconnect[1]));
  });

  REQUIRE(client.connect());
  REQUIRE(client.wasRecovered());
  server.join();

  // Pre-reset output was dropped by the reset (covered at the BackedIO
  // level); fresh data flows in both directions from here.

  client.writePacket(Packet(10, "hello"));
  Packet serverGot;
  REQUIRE(serverConn.readPacket(&serverGot));
  REQUIRE(serverGot.getPayload() == "hello");

  serverConn.writePacket(Packet(20, "hi-back"));
  Packet clientGot;
  REQUIRE(client.readPacket(&clientGot));
  REQUIRE(clientGot.getPayload() == "hi-back");

  client.shutdown();
  serverConn.shutdown();
  handler->close(live[0]);
  handler->close(reconnect[1]);
}
