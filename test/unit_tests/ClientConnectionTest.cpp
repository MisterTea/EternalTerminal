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

ConnectResponse authenticateKnownClient(
    const shared_ptr<SocketHandler>& handler, int fd, const string& clientId,
    const string& clientKey, string* proofOut = nullptr,
    string* challengeOut = nullptr) {
  ConnectRequest request;
  request.set_clientid(clientId);
  request.set_version(PROTOCOL_VERSION);
  request.set_supportschallenge(true);
  handler->writeProto(fd, request, true);

  const ConnectResponse challenge = handler->readProto<ConnectResponse>(
      fd, true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
  REQUIRE(challenge.has_authchallenge());
  REQUIRE(challenge.authchallenge().size() ==
          CryptoHandler::AUTH_CHALLENGE_BYTES);
  if (challengeOut != nullptr) {
    *challengeOut = challenge.authchallenge();
  }

  const string proof = CryptoHandler::connectionProof(
      clientKey, clientId, PROTOCOL_VERSION, challenge.authchallenge());
  if (proofOut != nullptr) {
    *proofOut = proof;
  }
  ConnectAuth auth;
  auth.set_proof(proof);
  handler->writeProto(fd, auth, true);
  return handler->readProto<ConnectResponse>(
      fd, true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
}

void finishRecoveryIfAttempted(const shared_ptr<SocketHandler>& handler, int fd,
                               bool* recoveryAttempted) {
  try {
    auto seqHeader = handler->readProto<SequenceHeader>(
        fd, true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
    (void)seqHeader;
    *recoveryAttempted = true;

    SequenceHeader seqResponse;
    seqResponse.set_sequencenumber(0);
    handler->writeProto(fd, seqResponse, true);
    auto catchup = handler->readProto<CatchupBuffer>(
        fd, true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
    (void)catchup;
    CatchupBuffer back;
    handler->writeProto(fd, back, true);
  } catch (const std::runtime_error&) {
  }
}

class HandshakeClientConnection : public ClientConnection {
 public:
  HandshakeClientConnection(shared_ptr<SocketHandler> handler, const string& id,
                            const string& key)
      : ClientConnection(handler, SocketEndpoint(), id, key) {}

  void handshake(int fd, ConnectResponse* response, bool resetIntent) {
    connectHandshake(fd, response, resetIntent);
  }
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

  bool recoverPublic(int fd, bool forceReset = false,
                     const string& resetSalt = string()) {
    return recover(fd, forceReset, resetSalt);
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
  ClientConnection conn(handler, SocketEndpoint(), "client-id", key,
                        /*_resetIntent=*/true);

  std::thread server([&]() {
    auto request = handler->readProto<ConnectRequest>(fds[1], true);
    REQUIRE(request.clientid() == "client-id");
    REQUIRE(request.version() == PROTOCOL_VERSION);
    REQUIRE(request.supportschallenge());

    ConnectResponse challenge;
    challenge.set_authchallenge(
        CryptoHandler::randomBytes(CryptoHandler::AUTH_CHALLENGE_BYTES));
    handler->writeProto(fds[1], challenge, true);
    auto auth = handler->readProto<ConnectAuth>(fds[1], true);
    REQUIRE(CryptoHandler::verifyConnectionProof(
        auth.proof(), key, "client-id", PROTOCOL_VERSION,
        challenge.authchallenge(), request.resetintent()));

    ConnectResponse response;
    response.set_status(RETURNING_CLIENT);
    response.set_resetrequired(request.resetintent());
    const string resetSalt =
        CryptoHandler::randomBytes(CryptoHandler::EPOCH_SALT_BYTES);
    response.set_resetsalt(resetSalt);
    response.set_resetproof(CryptoHandler::resetDecisionProof(
        key, "client-id", PROTOCOL_VERSION, challenge.authchallenge(),
        RETURNING_CLIENT, request.resetintent(), resetSalt));
    handler->writeProto(fds[1], response, true);

    auto seqHeader = handler->readProto<SequenceHeader>(
        fds[1], true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
    REQUIRE(seqHeader.reset());
    REQUIRE(seqHeader.resetsalt() == resetSalt);
    SequenceHeader seqResponse;
    seqResponse.set_sequencenumber(0);
    seqResponse.set_reset(true);
    seqResponse.set_resetsalt(resetSalt);
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

TEST_CASE("ClientConnection accepts a legacy server's unchallenged status",
          "[ClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  int fds[2];
  REQUIRE(createTestSocketPair(fds) == 0);
  handler->queueConnectFd(fds[0]);

  const string key = "12345678901234567890123456789012";
  ClientConnection conn(handler, SocketEndpoint(), "client-id", key);

  std::thread server([&]() {
    auto request = handler->readProto<ConnectRequest>(fds[1], true);
    REQUIRE(request.supportschallenge());
    ConnectResponse response;
    response.set_status(NEW_CLIENT);
    handler->writeProto(fds[1], response, true);
  });

  REQUIRE(conn.connect());
  server.join();
  REQUIRE(conn.lastStatus() == NEW_CLIENT);
  REQUIRE_FALSE(conn.wasRecovered());
  REQUIRE_FALSE(handler->hasData(fds[1]));

  conn.shutdown();
  handler->close(fds[1]);
}

TEST_CASE("ClientConnection refuses to reattach through a legacy server",
          "[ClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  int fds[2];
  REQUIRE(createTestSocketPair(fds) == 0);
  handler->queueConnectFd(fds[0]);

  const string key = "12345678901234567890123456789012";
  ClientConnection conn(handler, SocketEndpoint(), "client-id", key,
                        /*_resetIntent=*/true);

  std::thread server([&]() {
    auto request = handler->readProto<ConnectRequest>(fds[1], true);
    REQUIRE(request.resetintent());
    ConnectResponse response;
    response.set_status(RETURNING_CLIENT);
    response.set_resetrequired(true);
    response.set_resetsalt(
        CryptoHandler::randomBytes(CryptoHandler::EPOCH_SALT_BYTES));
    handler->writeProto(fds[1], response, true);
  });

  REQUIRE_THROWS_WITH(conn.connect(),
                      ClientConnection::LEGACY_SERVER_REATTACH_ERROR);
  server.join();
  REQUIRE_FALSE(conn.wasRecovered());
  char unexpected = 0;
  REQUIRE(::recv(fds[1], &unexpected, 1, 0) == 0);

  conn.shutdown();
  handler->close(fds[1]);
}

TEST_CASE(
    "ClientConnection accepts a legacy RETURNING_CLIENT on a fresh connect",
    "[ClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  int fds[2];
  REQUIRE(createTestSocketPair(fds) == 0);
  handler->queueConnectFd(fds[0]);

  const string key = "12345678901234567890123456789012";
  ClientConnection conn(handler, SocketEndpoint(), "client-id", key);

  std::thread server([&]() {
    auto request = handler->readProto<ConnectRequest>(fds[1], true);
    REQUIRE_FALSE(request.resetintent());
    ConnectResponse response;
    response.set_status(RETURNING_CLIENT);
    handler->writeProto(fds[1], response, true);
  });

  REQUIRE(conn.connect());
  server.join();
  REQUIRE(conn.lastStatus() == RETURNING_CLIENT);
  REQUIRE_FALSE(conn.wasRecovered());

  conn.shutdown();
  handler->close(fds[1]);
}

TEST_CASE("ClientConnection reconnects to a legacy server without resetIntent",
          "[ClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  int fds[2];
  REQUIRE(createTestSocketPair(fds) == 0);

  const string key = "12345678901234567890123456789012";
  HandshakeClientConnection conn(handler, "client-id", key);

  std::thread server([&]() {
    auto request = handler->readProto<ConnectRequest>(fds[1], true);
    REQUIRE_FALSE(request.resetintent());
    ConnectResponse response;
    response.set_status(RETURNING_CLIENT);
    response.set_resetrequired(true);
    response.set_resetsalt(
        CryptoHandler::randomBytes(CryptoHandler::EPOCH_SALT_BYTES));
    handler->writeProto(fds[1], response, true);
  });

  ConnectResponse response;
  conn.handshake(fds[0], &response, /*resetIntent=*/false);
  server.join();
  REQUIRE(response.status() == RETURNING_CLIENT);
  REQUIRE_FALSE(response.resetrequired());
  REQUIRE_FALSE(response.has_resetsalt());

  handler->close(fds[0]);
  handler->close(fds[1]);
}

TEST_CASE("ClientConnection rejects a success without reset decision proof",
          "[ClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  int fds[2];
  REQUIRE(createTestSocketPair(fds) == 0);
  handler->queueConnectFd(fds[0]);

  const string key = "12345678901234567890123456789012";
  ClientConnection conn(handler, SocketEndpoint(), "client-id", key,
                        /*_resetIntent=*/true);
  bool recoveryAttempted = false;

  std::thread server([&]() {
    handler->readProto<ConnectRequest>(fds[1], true);
    ConnectResponse challenge;
    challenge.set_authchallenge(
        CryptoHandler::randomBytes(CryptoHandler::AUTH_CHALLENGE_BYTES));
    handler->writeProto(fds[1], challenge, true);
    handler->readProto<ConnectAuth>(fds[1], true);

    ConnectResponse response;
    response.set_status(RETURNING_CLIENT);
    response.set_resetrequired(true);
    handler->writeProto(fds[1], response, true);
    finishRecoveryIfAttempted(handler, fds[1], &recoveryAttempted);
  });

  const bool connected = conn.connect();
  server.join();
  REQUIRE_FALSE(connected);
  REQUIRE_FALSE(recoveryAttempted);

  conn.shutdown();
  handler->close(fds[1]);
}

TEST_CASE(
    "ClientConnection rejects a captured final response replayed on a fresh "
    "challenge",
    "[ClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  int fds[2];
  REQUIRE(createTestSocketPair(fds) == 0);
  handler->queueConnectFd(fds[0]);

  const string key = "12345678901234567890123456789012";
  ClientConnection conn(handler, SocketEndpoint(), "client-id", key,
                        /*_resetIntent=*/true);
  bool recoveryAttempted = false;

  const string capturedChallenge =
      CryptoHandler::randomBytes(CryptoHandler::AUTH_CHALLENGE_BYTES);
  const string capturedSalt =
      CryptoHandler::randomBytes(CryptoHandler::EPOCH_SALT_BYTES);
  ConnectResponse capturedResponse;
  capturedResponse.set_status(RETURNING_CLIENT);
  capturedResponse.set_resetrequired(true);
  capturedResponse.set_resetsalt(capturedSalt);
  capturedResponse.set_resetproof(CryptoHandler::resetDecisionProof(
      key, "client-id", PROTOCOL_VERSION, capturedChallenge, RETURNING_CLIENT,
      true, capturedSalt));
  REQUIRE(CryptoHandler::verifyResetDecisionProof(
      capturedResponse.resetproof(), key, "client-id", PROTOCOL_VERSION,
      capturedChallenge, RETURNING_CLIENT, true, capturedSalt));

  std::thread server([&]() {
    handler->readProto<ConnectRequest>(fds[1], true);

    ConnectResponse challenge;
    challenge.set_authchallenge(
        CryptoHandler::randomBytes(CryptoHandler::AUTH_CHALLENGE_BYTES));
    REQUIRE(challenge.authchallenge() != capturedChallenge);
    handler->writeProto(fds[1], challenge, true);
    handler->readProto<ConnectAuth>(fds[1], true);

    handler->writeProto(fds[1], capturedResponse, true);
    finishRecoveryIfAttempted(handler, fds[1], &recoveryAttempted);
  });

  const bool connected = conn.connect();
  server.join();
  REQUIRE_FALSE(connected);
  REQUIRE_FALSE(recoveryAttempted);

  conn.shutdown();
  handler->close(fds[1]);
}

TEST_CASE("ClientConnection accepts direct protocol mismatch errors",
          "[ClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  int fds[2];
  REQUIRE(createTestSocketPair(fds) == 0);
  handler->queueConnectFd(fds[0]);

  ClientConnection conn(handler, SocketEndpoint(), "client-id",
                        "12345678901234567890123456789012");

  std::thread server([&]() {
    handler->readProto<ConnectRequest>(fds[1], true);
    ConnectResponse response;
    response.set_status(MISMATCHED_PROTOCOL);
    response.set_error("legacy peer");
    handler->writeProto(fds[1], response, true);
  });

  const bool connected = conn.connect();
  server.join();
  REQUIRE_FALSE(connected);
  REQUIRE(conn.lastStatus() == MISMATCHED_PROTOCOL);

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
  server.expireGrace();
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

  int firstPair[2];
  REQUIRE(createTestSocketPair(firstPair) == 0);
  ConnectRequest missingKeyRequest;
  missingKeyRequest.set_clientid("missing");
  missingKeyRequest.set_version(PROTOCOL_VERSION);
  missingKeyRequest.set_supportschallenge(true);
  handler->writeProto(firstPair[0], missingKeyRequest, true);
  server.clientHandler(firstPair[1]);
  auto missingKeyResponse =
      handler->readProto<ConnectResponse>(firstPair[0], true);
  REQUIRE(missingKeyResponse.status() == RETRY_LATER);
  handler->close(firstPair[0]);
  handler->close(firstPair[1]);

  server.expireGrace();
  int gracePair[2];
  REQUIRE(createTestSocketPair(gracePair) == 0);
  handler->writeProto(gracePair[0], missingKeyRequest, true);
  server.clientHandler(gracePair[1]);
  auto expiredGraceResponse =
      handler->readProto<ConnectResponse>(gracePair[0], true);
  REQUIRE(expiredGraceResponse.status() == INVALID_KEY);
  handler->close(gracePair[0]);
  handler->close(gracePair[1]);

  int versionPair[2];
  REQUIRE(createTestSocketPair(versionPair) == 0);
  ConnectRequest oldVersionRequest;
  oldVersionRequest.set_clientid("client-one");
  oldVersionRequest.set_version(PROTOCOL_VERSION - 1);
  oldVersionRequest.set_supportschallenge(true);
  handler->writeProto(versionPair[0], oldVersionRequest, true);
  server.clientHandler(versionPair[1]);
  auto versionResponse =
      handler->readProto<ConnectResponse>(versionPair[0], true);
  REQUIRE(versionResponse.status() == MISMATCHED_PROTOCOL);
  REQUIRE_FALSE(versionResponse.has_authchallenge());
  handler->close(versionPair[0]);
  handler->close(versionPair[1]);

  // Known key path should trigger newClient callback and NEW_CLIENT status.
  const string clientKey = "0123456789abcdef0123456789abcdef";
  server.addClientKey("client-one", clientKey);
  int secondPair[2];
  REQUIRE(createTestSocketPair(secondPair) == 0);

  std::thread serverThread([&]() { server.clientHandler(secondPair[1]); });

  auto knownClientResponse =
      authenticateKnownClient(handler, secondPair[0], "client-one", clientKey);
  REQUIRE(knownClientResponse.status() == NEW_CLIENT);

  serverThread.join();
  REQUIRE(server.newClientCalled);
  REQUIRE(server.clientConnectionExists("client-one"));
  REQUIRE(server.tryGetClientConnection("client-one") == server.lastConnection);
  REQUIRE_FALSE(server.tryGetClientConnection("missing"));
  REQUIRE_FALSE(server.removeClient("missing-client"));
  REQUIRE(server.removeClient("client-one"));
  REQUIRE_FALSE(server.clientConnectionExists("client-one"));

  handler->close(secondPair[0]);
  handler->close(secondPair[1]);
  server.shutdown();
}

TEST_CASE("ServerConnection answers legacy clients with the protocol-6 flow",
          "[ServerConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  SocketEndpoint endpoint;
  endpoint.set_name("server");
  endpoint.set_port(0);
  RecordingServerConnection server(handler, endpoint);

  int unknownPair[2];
  REQUIRE(createTestSocketPair(unknownPair) == 0);
  ConnectRequest unknownRequest;
  unknownRequest.set_clientid("unknown");
  unknownRequest.set_version(PROTOCOL_VERSION);
  handler->writeProto(unknownPair[0], unknownRequest, true);
  server.clientHandler(unknownPair[1]);
  auto unknownResponse =
      handler->readProto<ConnectResponse>(unknownPair[0], true);
  REQUIRE(unknownResponse.status() == INVALID_KEY);
  handler->close(unknownPair[0]);
  handler->close(unknownPair[1]);

  const string clientKey = "0123456789abcdef0123456789abcdef";
  server.addClientKey("legacy", clientKey);
  ConnectRequest request;
  request.set_clientid("legacy");
  request.set_version(PROTOCOL_VERSION);
  request.set_resetintent(true);

  int newPair[2];
  REQUIRE(createTestSocketPair(newPair) == 0);
  handler->writeProto(newPair[0], request, true);
  server.clientHandler(newPair[1]);
  auto newResponse = handler->readProto<ConnectResponse>(newPair[0], true);
  REQUIRE(newResponse.status() == NEW_CLIENT);
  REQUIRE_FALSE(newResponse.has_authchallenge());
  REQUIRE_FALSE(newResponse.has_resetrequired());
  REQUIRE_FALSE(newResponse.has_resetsalt());
  REQUIRE_FALSE(newResponse.has_resetproof());
  REQUIRE(server.newClientCalled);

  int returnPair[2];
  REQUIRE(createTestSocketPair(returnPair) == 0);
  handler->writeProto(returnPair[0], request, true);
  std::thread client([&]() {
    auto response = handler->readProto<ConnectResponse>(returnPair[0], true);
    REQUIRE(response.status() == RETURNING_CLIENT);
    REQUIRE_FALSE(response.has_authchallenge());
    REQUIRE_FALSE(response.has_resetrequired());
    REQUIRE_FALSE(response.has_resetsalt());
    REQUIRE_FALSE(response.has_resetproof());
    auto seqHeader = handler->readProto<SequenceHeader>(
        returnPair[0], true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
    REQUIRE_FALSE(seqHeader.reset());
    SequenceHeader seqResponse;
    seqResponse.set_sequencenumber(0);
    handler->writeProto(returnPair[0], seqResponse, true);
    handler->readProto<CatchupBuffer>(returnPair[0], true);
    CatchupBuffer back;
    handler->writeProto(returnPair[0], back, true);
  });
  server.clientHandler(returnPair[1]);
  client.join();
  REQUIRE(server.clientConnectionExists("legacy"));

  server.shutdown();
  handler->close(newPair[0]);
  handler->close(returnPair[0]);
}

TEST_CASE("ServerConnection refuses to resume a legacy client",
          "[ServerConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  SocketEndpoint endpoint;
  endpoint.set_name("server");
  endpoint.set_port(0);
  RecordingServerConnection server(handler, endpoint);

  server.addClientKey("live-term", "0123456789abcdef0123456789abcdef");
  server.resumeIds.insert("live-term");

  int fds[2];
  REQUIRE(createTestSocketPair(fds) == 0);
  ConnectRequest request;
  request.set_clientid("live-term");
  request.set_version(PROTOCOL_VERSION);
  handler->writeProto(fds[0], request, true);
  server.clientHandler(fds[1]);

  auto response = handler->readProto<ConnectResponse>(fds[0], true);
  REQUIRE(response.status() == INVALID_KEY);
  REQUIRE_FALSE(response.has_authchallenge());
  REQUIRE_FALSE(response.has_resetrequired());
  REQUIRE_FALSE(server.resumeClientCalled);
  REQUIRE_FALSE(server.newClientCalled);
  REQUIRE_FALSE(server.clientConnectionExists("live-term"));
  REQUIRE(server.clientKeyExists("live-term"));

  server.shutdown();
  handler->close(fds[0]);
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
  REQUIRE(createTestSocketPair(endedPair) == 0);
  ConnectRequest endedRequest;
  endedRequest.set_clientid("ended");
  endedRequest.set_version(PROTOCOL_VERSION);
  endedRequest.set_supportschallenge(true);
  handler->writeProto(endedPair[0], endedRequest, true);
  server.clientHandler(endedPair[1]);
  auto endedResponse = handler->readProto<ConnectResponse>(endedPair[0], true);
  REQUIRE(endedResponse.status() == INVALID_KEY);
  handler->close(endedPair[0]);
  handler->close(endedPair[1]);

  int unknownPair[2];
  REQUIRE(createTestSocketPair(unknownPair) == 0);
  ConnectRequest unknownRequest;
  unknownRequest.set_clientid("unknown");
  unknownRequest.set_version(PROTOCOL_VERSION);
  unknownRequest.set_supportschallenge(true);
  handler->writeProto(unknownPair[0], unknownRequest, true);
  server.clientHandler(unknownPair[1]);
  auto unknownResponse =
      handler->readProto<ConnectResponse>(unknownPair[0], true);
  REQUIRE(unknownResponse.status() == RETRY_LATER);
  handler->close(unknownPair[0]);
  handler->close(unknownPair[1]);

  server.expireRemovedClient("ended");
  int expiredPair[2];
  REQUIRE(createTestSocketPair(expiredPair) == 0);
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

  const string clientKey = "0123456789abcdef0123456789abcdef";
  server.addClientKey("live-term", clientKey);
  server.resumeIds.insert("live-term");

  int fds[2];
  REQUIRE(createTestSocketPair(fds) == 0);

  std::thread client([&]() {
    string authChallenge;
    auto response = authenticateKnownClient(handler, fds[0], "live-term",
                                            clientKey, nullptr, &authChallenge);
    REQUIRE(response.status() == RETURNING_CLIENT);
    REQUIRE(response.resetrequired());
    REQUIRE(response.resetsalt().size() == CryptoHandler::EPOCH_SALT_BYTES);
    REQUIRE(response.has_resetproof());
    REQUIRE(CryptoHandler::verifyResetDecisionProof(
        response.resetproof(), clientKey, "live-term", PROTOCOL_VERSION,
        authChallenge, RETURNING_CLIENT, true, response.resetsalt()));

    auto seqHeader = handler->readProto<SequenceHeader>(
        fds[0], true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
    REQUIRE(seqHeader.sequencenumber() == 0);
    REQUIRE(seqHeader.reset());
    REQUIRE(seqHeader.resetsalt() == response.resetsalt());
    SequenceHeader seqResponse;
    seqResponse.set_sequencenumber(0);
    seqResponse.set_reset(true);
    seqResponse.set_resetsalt(response.resetsalt());
    handler->writeProto(fds[0], seqResponse, true);
    auto catchup = handler->readProto<CatchupBuffer>(fds[0], true);
    REQUIRE(catchup.buffer_size() == 0);
    CatchupBuffer back;
    handler->writeProto(fds[0], back, true);
  });

  server.clientHandler(fds[1]);
  client.join();

  REQUIRE(server.resumeClientCalled);
  REQUIRE_FALSE(server.newClientCalled);
  REQUIRE(server.clientConnectionExists("live-term"));

  server.shutdown();
  handler->close(fds[0]);
  handler->close(fds[1]);
}

TEST_CASE("ServerConnection rejects known ids without proof before reset",
          "[ServerConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  SocketEndpoint endpoint;
  endpoint.set_name("server");
  endpoint.set_port(0);
  RecordingServerConnection server(handler, endpoint);

  const string clientKey = "0123456789abcdef0123456789abcdef";
  server.addClientKey("live-term", clientKey);
  server.resumeIds.insert("live-term");

  int fds[2];
  REQUIRE(createTestSocketPair(fds) == 0);
  ConnectRequest request;
  request.set_clientid("live-term");
  request.set_version(PROTOCOL_VERSION);
  request.set_supportschallenge(true);
  handler->writeProto(fds[0], request, true);

  std::thread serverThread([&]() { server.clientHandler(fds[1]); });

  auto challenge = handler->readProto<ConnectResponse>(fds[0], true);
  REQUIRE(challenge.has_authchallenge());
  ConnectAuth emptyAuth;
  handler->writeProto(fds[0], emptyAuth, true);

  auto response = handler->readProto<ConnectResponse>(fds[0], true);
  REQUIRE(response.status() == INVALID_KEY);
  REQUIRE_FALSE(server.resumeClientCalled);
  REQUIRE_FALSE(server.clientConnectionExists("live-term"));

  serverThread.join();
  handler->close(fds[0]);
  server.shutdown();
}

TEST_CASE("ServerConnection rejects a proof replayed for an old challenge",
          "[ServerConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  SocketEndpoint endpoint;
  endpoint.set_name("server");
  endpoint.set_port(0);
  RecordingServerConnection server(handler, endpoint);

  const string clientId = "live-term";
  const string clientKey = "0123456789abcdef0123456789abcdef";
  server.addClientKey(clientId, clientKey);

  const string oldChallenge =
      CryptoHandler::randomBytes(CryptoHandler::AUTH_CHALLENGE_BYTES);
  const string oldProof = CryptoHandler::connectionProof(
      clientKey, clientId, PROTOCOL_VERSION, oldChallenge);

  int fds[2];
  REQUIRE(createTestSocketPair(fds) == 0);
  std::thread serverThread([&]() { server.clientHandler(fds[1]); });

  ConnectRequest request;
  request.set_clientid(clientId);
  request.set_version(PROTOCOL_VERSION);
  request.set_supportschallenge(true);
  handler->writeProto(fds[0], request, true);
  auto challenge = handler->readProto<ConnectResponse>(fds[0], true);
  REQUIRE(challenge.has_authchallenge());
  REQUIRE(challenge.authchallenge() != oldChallenge);

  ConnectAuth replay;
  replay.set_proof(oldProof);
  handler->writeProto(fds[0], replay, true);
  auto response = handler->readProto<ConnectResponse>(fds[0], true);
  REQUIRE(response.status() == INVALID_KEY);
  REQUIRE_FALSE(server.clientConnectionExists(clientId));

  serverThread.join();
  handler->close(fds[0]);
  server.shutdown();
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
  const string resetSalt = "0123456789abcdef0123456789abcdef";

  std::thread remote([&]() {
    auto seqHeader = handler->readProto<SequenceHeader>(
        reconnect[1], true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
    REQUIRE(seqHeader.sequencenumber() == 0);
    REQUIRE(seqHeader.reset());
    REQUIRE(seqHeader.resetsalt() == resetSalt);

    SequenceHeader seqResponse;
    seqResponse.set_sequencenumber(5);
    seqResponse.set_reset(true);
    seqResponse.set_resetsalt(resetSalt);
    handler->writeProto(reconnect[1], seqResponse, true);

    auto catchup = handler->readProto<CatchupBuffer>(reconnect[1], true);
    REQUIRE(catchup.buffer_size() == 0);
    CatchupBuffer back;
    handler->writeProto(reconnect[1], back, true);
  });

  REQUIRE(conn.recoverPublic(reconnect[0], true, resetSalt));
  remote.join();

  REQUIRE(conn.getReader()->getSequenceNumber() == 0);
  REQUIRE(conn.getWriter()->getSequenceNumber() == 0);
  conn.write(Packet(3, "after-reset"));
  REQUIRE(conn.getWriter()->getSequenceNumber() == 1);
}

TEST_CASE("Connection rejects an unsolicited reset request", "[Connection]") {
  auto handler = make_shared<SocketPairHandler>();
  int live[2];
  REQUIRE(createTestSocketPair(live) == 0);

  const string key = "zyxwvutsrqponmlkjihgfedcba987654";
  auto encryptCrypto = make_shared<CryptoHandler>(key, 0);
  auto decryptCrypto = make_shared<CryptoHandler>(key, 0);
  auto reader = make_shared<BackedReader>(handler, decryptCrypto, live[0]);
  auto writer = make_shared<BackedWriter>(handler, encryptCrypto, live[0]);
  RecoverableConnection conn(handler, reader, writer, live[0], key);
  conn.closeSocket();

  int reconnect[2];
  REQUIRE(createTestSocketPair(reconnect) == 0);
  std::thread remote([&]() {
    handler->readProto<SequenceHeader>(
        reconnect[1], true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
    SequenceHeader forged;
    forged.set_reset(true);
    forged.set_resetsalt(string(CryptoHandler::EPOCH_SALT_BYTES, 'x'));
    handler->writeProto(reconnect[1], forged, true);
  });

  REQUIRE_FALSE(conn.recoverPublic(reconnect[0]));
  remote.join();
  conn.shutdown();
  handler->close(live[1]);
  handler->close(reconnect[0]);
  handler->close(reconnect[1]);
}

TEST_CASE("Connection rejects a reset header with a tampered salt",
          "[Connection]") {
  auto handler = make_shared<SocketPairHandler>();
  int live[2];
  REQUIRE(createTestSocketPair(live) == 0);

  const string key = "zyxwvutsrqponmlkjihgfedcba987654";
  auto reader = make_shared<BackedReader>(
      handler, make_shared<CryptoHandler>(key, 0), live[0]);
  auto writer = make_shared<BackedWriter>(
      handler, make_shared<CryptoHandler>(key, 0), live[0]);
  RecoverableConnection conn(handler, reader, writer, live[0], key);
  conn.closeSocket();

  int reconnect[2];
  REQUIRE(createTestSocketPair(reconnect) == 0);
  const string negotiatedSalt = "0123456789abcdef0123456789abcdef";
  const string tamperedSalt = "fedcba9876543210fedcba9876543210";
  atomic<bool> tamperAccepted(false);

  std::thread remote([&]() {
    auto localHeader = handler->readProto<SequenceHeader>(
        reconnect[1], true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
    REQUIRE(localHeader.reset());
    REQUIRE(localHeader.resetsalt() == negotiatedSalt);

    SequenceHeader tampered;
    tampered.set_sequencenumber(0);
    tampered.set_reset(true);
    tampered.set_resetsalt(tamperedSalt);
    handler->writeProto(reconnect[1], tampered, true);

    try {
      handler->readProto<CatchupBuffer>(reconnect[1], true);
      tamperAccepted.store(true);
    } catch (const std::runtime_error&) {
    }
  });

  REQUIRE_FALSE(conn.recoverPublic(reconnect[0], true, negotiatedSalt));
  remote.join();
  REQUIRE_FALSE(tamperAccepted.load());

  conn.shutdown();
  handler->close(live[1]);
  handler->close(reconnect[1]);
}

TEST_CASE(
    "ClientConnection connect performs reset recovery for returning "
    "clients",
    "[ClientConnection]") {
  auto handler = make_shared<SocketPairHandler>();
  const string key = "zyxwvutsrqponmlkjihgfedcba987654";

  int live[2];
  REQUIRE(createTestSocketPair(live) == 0);
  ServerClientConnection serverConn(handler, "client-id", live[0], key);
  serverConn.writePacket(Packet(1, "pre-existing-output"));

  handler->close(live[1]);

  int reconnect[2];
  REQUIRE(createTestSocketPair(reconnect) == 0);
  handler->queueConnectFd(reconnect[0]);
  ClientConnection client(handler, SocketEndpoint(), "client-id", key,
                          /*_resetIntent=*/true);

  std::thread server([&]() {
    auto request = handler->readProto<ConnectRequest>(reconnect[1], true);
    REQUIRE(request.clientid() == "client-id");
    ConnectResponse challenge;
    challenge.set_authchallenge(
        CryptoHandler::randomBytes(CryptoHandler::AUTH_CHALLENGE_BYTES));
    handler->writeProto(reconnect[1], challenge, true);
    auto auth = handler->readProto<ConnectAuth>(reconnect[1], true);
    REQUIRE(CryptoHandler::verifyConnectionProof(
        auth.proof(), key, "client-id", PROTOCOL_VERSION,
        challenge.authchallenge(), request.resetintent()));
    ConnectResponse response;
    response.set_status(RETURNING_CLIENT);
    response.set_resetrequired(request.resetintent());
    const string resetSalt =
        CryptoHandler::randomBytes(CryptoHandler::EPOCH_SALT_BYTES);
    response.set_resetsalt(resetSalt);
    response.set_resetproof(CryptoHandler::resetDecisionProof(
        key, "client-id", PROTOCOL_VERSION, challenge.authchallenge(),
        RETURNING_CLIENT, request.resetintent(), resetSalt));
    handler->writeProto(reconnect[1], response, true);
    REQUIRE(serverConn.recoverClient(reconnect[1], request.resetintent(),
                                     resetSalt));
  });

  REQUIRE(client.connect());
  REQUIRE(client.wasRecovered());
  server.join();

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
