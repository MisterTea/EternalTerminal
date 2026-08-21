#include <queue>
#include <set>

#include "ClientConnection.hpp"
#include "ServerClientConnection.hpp"
#include "ServerConnection.hpp"
#include "TestHeaders.hpp"
#include "TestSocketPair.hpp"

namespace et {
namespace {
// Minimal socket handler that works with socketpairs for handshake tests.
class SocketPairHandler : public SocketHandler {
 public:
  void queueConnectFd(int fd) { connectQueue.push(fd); }
  void queueAcceptFd(int fd) { acceptQueue.push(fd); }

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
  int accept(int) override {
    if (acceptQueue.empty()) {
      SetErrno(EAGAIN);
      return -1;
    }
    int fd = acceptQueue.front();
    acceptQueue.pop();
    return fd;
  }
  void stopListening(const SocketEndpoint&) override {}
  void close(int fd) override {
    lock_guard<std::mutex> guard(closeCountsMutex);
    closeCounts[fd]++;
    ::close(fd);
  }
  vector<int> getActiveSockets() override { return {}; }
  int closeCount(int fd) const {
    lock_guard<std::mutex> guard(closeCountsMutex);
    auto it = closeCounts.find(fd);
    return it == closeCounts.end() ? 0 : it->second;
  }

 private:
  std::queue<int> connectQueue;
  std::queue<int> acceptQueue;
  mutable std::mutex closeCountsMutex;
  std::map<int, int> closeCounts;
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

  // Pretend the recovery grace window has long passed.
  void expireGrace() { startTime_ = time(NULL) - recoveryGraceSeconds - 1; }

  void expireRemovedClient(const string& id) {
    removedClientIds.at(id) = time(NULL) - recoveryGraceSeconds - 1;
  }

  size_t removedClientCount() const { return removedClientIds.size(); }

  bool shouldResumeAsReturning(const string& clientId) override {
    return resumeIds.count(clientId) > 0;
  }

  void resumeClient(shared_ptr<ServerClientConnection> state) override {
    resumeClientCalled = true;
    lastConnection = std::move(state);
  }

  bool newClientCalled = false;
  bool allowNewClients = true;
  shared_ptr<ServerClientConnection> lastConnection;
  std::set<string> resumeIds;
  bool resumeClientCalled = false;
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
using namespace et::test;

TEST_CASE("ClientConnection completes handshake over socketpair",
          "[ClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  int fds[2];
  REQUIRE(createTestSocketPair(fds) == 0);
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
    REQUIRE(seqHeader.resetsalt().size() == CryptoHandler::EPOCH_SALT_BYTES);
    SequenceHeader seqResponse;
    seqResponse.set_sequencenumber(0);
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
  REQUIRE(createTestSocketPair(fds) == 0);
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

  REQUIRE(conn.getSocketFd() == -1);
  REQUIRE(handler->closeCount(fds[0]) == 1);

  conn.shutdown();
  handler->close(fds[1]);
}

TEST_CASE("ClientConnection reports an unavailable endpoint",
          "[ClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  ClientConnection conn(handler, SocketEndpoint(), "client-id",
                        "12345678901234567890123456789012");
  REQUIRE_FALSE(conn.connect());
  conn.shutdown();
}

TEST_CASE("ServerConnection accepts queued clients", "[ServerConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  RecordingServerConnection server(handler, SocketEndpoint());
  REQUIRE_FALSE(server.acceptNewConnection(123));

  int pair[2];
  REQUIRE(createTestSocketPair(pair) == 0);
  ConnectRequest request;
  request.set_clientid("unknown-client");
  request.set_version(PROTOCOL_VERSION);
  handler->writeProto(pair[0], request, true);
  handler->queueAcceptFd(pair[1]);
  REQUIRE(server.acceptNewConnection(123));

  auto response = handler->readProto<ConnectResponse>(pair[0], true);
  REQUIRE(response.status() == INVALID_KEY);
  handler->close(pair[0]);
  server.shutdown();
}

TEST_CASE("ServerConnection responds to known and unknown clients",
          "[ServerConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  SocketEndpoint endpoint;
  endpoint.set_name("server");
  endpoint.set_port(0);
  RecordingServerConnection server(handler, endpoint);

  // Missing key path returns RETRY_LATER while the server is still within
  // its post-startup recovery grace window.
  int firstPair[2];
  REQUIRE(createTestSocketPair(firstPair) == 0);
  ConnectRequest missingKeyRequest;
  missingKeyRequest.set_clientid("missing");
  missingKeyRequest.set_version(PROTOCOL_VERSION);
  handler->writeProto(firstPair[0], missingKeyRequest, true);
  server.clientHandler(firstPair[1]);
  auto missingKeyResponse =
      handler->readProto<ConnectResponse>(firstPair[0], true);
  REQUIRE(missingKeyResponse.status() == RETRY_LATER);
  handler->close(firstPair[0]);
  handler->close(firstPair[1]);

  // Past the grace window an unknown id is a hard INVALID_KEY.
  server.expireGrace();
  int gracePair[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, gracePair) == 0);
  handler->writeProto(gracePair[0], missingKeyRequest, true);
  server.clientHandler(gracePair[1]);
  auto expiredGraceResponse =
      handler->readProto<ConnectResponse>(gracePair[0], true);
  REQUIRE(expiredGraceResponse.status() == INVALID_KEY);
  handler->close(gracePair[0]);
  handler->close(gracePair[1]);

  // Known key path should trigger newClient callback and NEW_CLIENT status.
  const string clientKey = "0123456789abcdef0123456789abcdef";
  server.addClientKey("client-one", clientKey);
  int secondPair[2];
  REQUIRE(createTestSocketPair(secondPair) == 0);

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
  REQUIRE_FALSE(server.removeClient("missing-client"));
  REQUIRE(server.removeClient("client-one"));
  REQUIRE_FALSE(server.clientConnectionExists("client-one"));

  handler->close(secondPair[0]);
  handler->close(secondPair[1]);
  server.shutdown();
}

TEST_CASE("ServerConnection rejects removed clients during recovery grace",
          "[ServerConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  SocketEndpoint endpoint;
  endpoint.set_name("server");
  endpoint.set_port(0);
  RecordingServerConnection server(handler, endpoint);

  server.addClientKey("ended", "0123456789abcdef0123456789abcdef");
  REQUIRE(server.removeClient("ended"));

  int endedPair[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, endedPair) == 0);
  ConnectRequest endedRequest;
  endedRequest.set_clientid("ended");
  endedRequest.set_version(PROTOCOL_VERSION);
  handler->writeProto(endedPair[0], endedRequest, true);
  server.clientHandler(endedPair[1]);
  auto endedResponse = handler->readProto<ConnectResponse>(endedPair[0], true);
  REQUIRE(endedResponse.status() == INVALID_KEY);
  handler->close(endedPair[0]);
  handler->close(endedPair[1]);

  int unknownPair[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, unknownPair) == 0);
  ConnectRequest unknownRequest;
  unknownRequest.set_clientid("unknown");
  unknownRequest.set_version(PROTOCOL_VERSION);
  handler->writeProto(unknownPair[0], unknownRequest, true);
  server.clientHandler(unknownPair[1]);
  auto unknownResponse =
      handler->readProto<ConnectResponse>(unknownPair[0], true);
  REQUIRE(unknownResponse.status() == RETRY_LATER);
  handler->close(unknownPair[0]);
  handler->close(unknownPair[1]);

  // Once the removal marker itself expires, the id is unknown again. Because
  // this server is still inside its startup grace, unknown ids retry instead
  // of being treated as sessions that definitely ended.
  server.expireRemovedClient("ended");
  int expiredPair[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, expiredPair) == 0);
  handler->writeProto(expiredPair[0], endedRequest, true);
  server.clientHandler(expiredPair[1]);
  auto expiredResponse =
      handler->readProto<ConnectResponse>(expiredPair[0], true);
  REQUIRE(expiredResponse.status() == RETRY_LATER);
  REQUIRE(server.removedClientCount() == 0);
  handler->close(expiredPair[0]);
  handler->close(expiredPair[1]);

  server.shutdown();
}

TEST_CASE("ServerConnection resumes sessions with an active pty",
          "[ServerConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  SocketEndpoint endpoint;
  endpoint.set_name("server");
  endpoint.set_port(0);
  RecordingServerConnection server(handler, endpoint);

  // The terminal re-registered with a live pty: the key exists, no
  // connection survived, and shouldResumeAsReturning says the pty is active.
  const string clientKey = "0123456789abcdef0123456789abcdef";
  server.addClientKey("live-term", clientKey);
  server.resumeIds.insert("live-term");

  int fds[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  std::thread client([&]() {
    ConnectRequest request;
    request.set_clientid("live-term");
    request.set_version(PROTOCOL_VERSION);
    handler->writeProto(fds[0], request, true);

    auto response = handler->readProto<ConnectResponse>(fds[0], true);
    REQUIRE(response.status() == RETURNING_CLIENT);

    // The server initiates the reset recovery exchange.
    auto seqHeader = handler->readProto<SequenceHeader>(
        fds[0], true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
    REQUIRE(seqHeader.sequencenumber() == 0);
    REQUIRE(seqHeader.reset());
    REQUIRE(seqHeader.resetsalt().size() == CryptoHandler::EPOCH_SALT_BYTES);
    SequenceHeader seqResponse;
    seqResponse.set_sequencenumber(0);
    handler->writeProto(fds[0], seqResponse, true);
    auto catchup = handler->readProto<CatchupBuffer>(fds[0], true);
    REQUIRE(catchup.buffer_size() == 0);
    CatchupBuffer back;
    handler->writeProto(fds[0], back, true);
  });

  server.clientHandler(fds[1]);
  client.join();

  // Resume path taken, not the fresh-bootstrap newClient path.
  REQUIRE(server.resumeClientCalled);
  REQUIRE_FALSE(server.newClientCalled);
  REQUIRE(server.clientConnectionExists("live-term"));

  server.shutdown();
  handler->close(fds[0]);
  handler->close(fds[1]);
}

TEST_CASE("ServerClientConnection verifies passkeys",
          "[ServerClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  int fds[2];
  REQUIRE(createTestSocketPair(fds) == 0);

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
  REQUIRE(createTestSocketPair(live) == 0);

  const string key = "zyxwvutsrqponmlkjihgfedcba987654";
  ServerClientConnection connection(handler, "client-recover", live[0], key);
  REQUIRE(connection.getSocketFd() == live[0]);

  int attack[2];
  REQUIRE(createTestSocketPair(attack) == 0);

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
  REQUIRE(createTestSocketPair(live) == 0);

  const string key = "zyxwvutsrqponmlkjihgfedcba987654";
  ServerClientConnection connection(handler, "client-recover-ok", live[0], key);
  REQUIRE(connection.getSocketFd() == live[0]);

  int reconnect[2];
  REQUIRE(createTestSocketPair(reconnect) == 0);

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
  REQUIRE(createTestSocketPair(live) == 0);

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
  REQUIRE(createTestSocketPair(reconnect) == 0);

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
    REQUIRE(seqHeader.resetsalt().size() == CryptoHandler::EPOCH_SALT_BYTES);

    // The remote peer is further ahead; with a reset its history is
    // discarded, so this must not trigger a "client is ahead" failure.
    SequenceHeader seqResponse;
    seqResponse.set_sequencenumber(5);
    seqResponse.set_reset(true);
    seqResponse.set_resetsalt(string(CryptoHandler::EPOCH_SALT_BYTES, 'r'));
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
