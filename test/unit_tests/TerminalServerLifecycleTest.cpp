#ifndef WIN32
#include <future>

#include "TerminalServer.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {
class LifecycleClient : public ServerClientConnection {
 public:
  LifecycleClient(shared_ptr<SocketHandler> socketHandler, const string& id,
                  const string& key)
      : ServerClientConnection(socketHandler, id, -1, key) {}

  bool readPacket(Packet* packet) override {
    *packet = Packet(EtPacketType::INITIAL_PAYLOAD, protoToString(payload));
    return true;
  }

  void writePacket(const Packet& packet) override {
    if (failInitialResponse &&
        packet.getHeader() == EtPacketType::INITIAL_RESPONSE) {
      throw std::runtime_error("Initial response failed");
    }
    if (packet.getHeader() == TerminalPacketType::TERMINAL_BUFFER) {
      terminalOutput.set_value(
          stringToProto<TerminalBuffer>(packet.getPayload()).buffer());
    }
  }

  InitialPayload payload;
  bool failInitialResponse = false;
  std::promise<string> terminalOutput;
};

class LifecycleServer : public TerminalServer {
 public:
  using TerminalServer::TerminalServer;

  void registerClient(const shared_ptr<ServerClientConnection>& client,
                      const string& key) {
    addClientKey(client->getId(), key);
    clientConnections[client->getId()] = client;
  }

  void dropConnection(const string& id) { clientConnections.erase(id); }

  void setRecoveryGraceSeconds(int seconds) { recoveryGraceSeconds = seconds; }
};

class TerminalSessionFixture {
 public:
  TerminalSessionFixture() {
    string pattern = GetTempDirectory() + "et_session_test_XXXXXX";
    char* directory = mkdtemp(&pattern[0]);
    REQUIRE(directory != nullptr);
    pipeDirectory = directory;
    serverEndpoint.set_name(pipeDirectory + "/server");
    routerEndpoint.set_name(pipeDirectory + "/router");
    server = std::make_shared<LifecycleServer>(networkHandler, serverEndpoint,
                                               routerHandler, routerEndpoint);
    client = std::make_shared<LifecycleClient>(networkHandler, id, key);
    server->registerClient(client, key);
    registerTerminal();
  }

  ~TerminalSessionFixture() {
    server->shutdown();
    closeTerminal();
    client->shutdown();
    joinSession();
    for (int fd : routerHandler->getActiveSockets()) {
      routerHandler->close(fd);
    }
    server->ServerConnection::shutdown();
    routerHandler->stopListening(routerEndpoint);
    ::remove(serverEndpoint.name().c_str());
    ::remove(routerEndpoint.name().c_str());
    ::remove(pipeDirectory.c_str());
  }

  void registerTerminal(bool ptyActive = false) {
    terminalPeer = peerHandler->connect(routerEndpoint);
    REQUIRE(terminalPeer >= 0);
    TerminalUserInfo userInfo;
    userInfo.set_id(id);
    userInfo.set_passkey(key);
    userInfo.set_uid(getuid());
    userInfo.set_gid(getgid());
    userInfo.set_ptyactive(ptyActive);
    peerHandler->writePacket(terminalPeer,
                             Packet(TerminalPacketType::TERMINAL_USER_INFO,
                                    protoToString(userInfo)));
    REQUIRE(server->terminalRouter->acceptNewConnection().id == id);
    auto info = server->terminalRouter->tryGetInfoForConnection(client);
    REQUIRE(info.has_value());
    terminalFd = info->fd();
  }

  void startSession() { REQUIRE(server->newClient(client)); }

  void waitForInit() {
    REQUIRE(peerHandler->waitForData(terminalPeer, 2, 0));
    Packet packet;
    REQUIRE(peerHandler->readPacket(terminalPeer, &packet));
    CHECK(packet.getHeader() == (client->payload.jumphost()
                                     ? TerminalPacketType::JUMPHOST_INIT
                                     : TerminalPacketType::TERMINAL_INIT));
  }

  void closeTerminal() {
    if (terminalPeer >= 0) {
      peerHandler->close(terminalPeer);
      terminalPeer = -1;
    }
  }

  void joinSession() {
    for (const auto& thread : server->terminalThreads) {
      if (thread->joinable()) {
        thread->join();
      }
    }
  }

  void waitForSessionEnd() {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server->clientConnectionExists(id) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_FALSE(server->clientConnectionExists(id));
    joinSession();
  }

  void reregisterWithoutClient() {
    server->dropConnection(id);
    closeTerminal();
    registerTerminal(/*ptyActive=*/true);
  }

  bool terminalReceivedClose() {
    if (!peerHandler->waitForData(terminalPeer, 2, 0)) {
      return false;
    }
    char packetType = 0;
    return peerHandler->read(terminalPeer, &packetType, 1) == 1 &&
           packetType == TERMINAL_CLOSE;
  }

  void checkSessionClosed() {
    CHECK(fcntl(terminalFd, F_GETFD) == -1);
    CHECK(errno == EBADF);
    CHECK_FALSE(server->clientKeyExists(id));
    CHECK_FALSE(server->clientConnectionExists(id));
  }

  const string id = "session-test";
  const string key = string(32, 'k');
  shared_ptr<PipeSocketHandler> networkHandler =
      std::make_shared<PipeSocketHandler>();
  shared_ptr<PipeSocketHandler> routerHandler =
      std::make_shared<PipeSocketHandler>();
  shared_ptr<PipeSocketHandler> peerHandler =
      std::make_shared<PipeSocketHandler>();
  shared_ptr<LifecycleServer> server;
  shared_ptr<LifecycleClient> client;
  SocketEndpoint serverEndpoint;
  SocketEndpoint routerEndpoint;
  string pipeDirectory;
  int terminalPeer = -1;
  int terminalFd = -1;
};
}  // namespace

TEST_CASE_METHOD(TerminalSessionFixture,
                 "Finished sessions release their router connection",
                 "[TerminalServerLifecycle]") {
  SECTION("Terminal") { client->payload.set_jumphost(false); }
  SECTION("Jumphost") { client->payload.set_jumphost(true); }
  startSession();
  waitForInit();
  closeTerminal();
  waitForSessionEnd();
  checkSessionClosed();
  registerTerminal();
}

TEST_CASE_METHOD(TerminalSessionFixture,
                 "Session setup exceptions release their router connection",
                 "[TerminalServerLifecycle]") {
  SECTION("Terminal") { client->payload.set_jumphost(false); }
  SECTION("Jumphost") { client->payload.set_jumphost(true); }
  client->failInitialResponse = true;
  startSession();
  waitForSessionEnd();
  checkSessionClosed();
}

TEST_CASE_METHOD(TerminalSessionFixture,
                 "Rejected forwarding setup releases the session",
                 "[TerminalServerLifecycle]") {
  auto* request = client->payload.add_reversetunnels();
  request->mutable_source()->set_port(8080);
  request->set_environmentvariable("TEST_FORWARD");
  startSession();
  waitForSessionEnd();
  checkSessionClosed();
}

TEST_CASE_METHOD(TerminalSessionFixture,
                 "Failed router authentication preserves the registered route",
                 "[TerminalServerLifecycle]") {
  auto wrongClient =
      std::make_shared<LifecycleClient>(networkHandler, id, string(32, 'x'));
  server->registerClient(wrongClient, string(32, 'x'));
  REQUIRE(server->newClient(wrongClient));
  waitForSessionEnd();
  CHECK(fcntl(terminalFd, F_GETFD) >= 0);
  auto info = server->terminalRouter->tryGetInfoForConnection(client);
  REQUIRE(info.has_value());
  CHECK(info->fd() == terminalFd);
  CHECK(wrongClient->isShuttingDown());
}

TEST_CASE_METHOD(TerminalSessionFixture,
                 "Permanent client shutdown releases the session",
                 "[TerminalServerLifecycle]") {
  startSession();
  waitForInit();
  client->shutdown();
  waitForSessionEnd();
  checkSessionClosed();
}

TEST_CASE("a forward wall-clock jump does not expire a fresh disconnect",
          "[TerminalServerLifecycle]") {
  DisconnectDeadline deadline;
  const auto start =
      std::chrono::steady_clock::time_point(std::chrono::seconds(1000000));
  CHECK_FALSE(disconnectDeadlineReached(&deadline, start, false, 3600));
  CHECK(deadline.started);
  // Ten steady seconds must not count as the old wall-clock hour jump.
  CHECK_FALSE(disconnectDeadlineReached(
      &deadline, start + std::chrono::seconds(10), false, 3600));
  CHECK(disconnectDeadlineReached(&deadline, start + std::chrono::seconds(3600),
                                  false, 3600));
}

TEST_CASE("disconnect expiry rechecks a socket that reconnected",
          "[TerminalServerLifecycle]") {
  DisconnectDeadline deadline;
  const auto start =
      std::chrono::steady_clock::time_point(std::chrono::seconds(1000));
  CHECK_FALSE(disconnectDeadlineReached(&deadline, start, false, 1));
  // Stale snapshot says disconnected and the timeout has elapsed, but the
  // live fd is connected again.
  CHECK_FALSE(disconnectExpiryClosesSession(
      -1, 8, false, &deadline, start + std::chrono::seconds(2), 1));
  CHECK_FALSE(disconnectExpiryClosesSession(
      -1, -1, true, &deadline, start + std::chrono::seconds(2), 1));
  CHECK(disconnectExpiryClosesSession(-1, -1, false, &deadline,
                                      start + std::chrono::seconds(2), 1));
}

TEST_CASE("disconnect deadline stays idle until the timeout elapses",
          "[TerminalServerLifecycle]") {
  DisconnectDeadline deadline;
  const auto t0 =
      std::chrono::steady_clock::time_point(std::chrono::seconds(1000));
  CHECK_FALSE(disconnectDeadlineReached(&deadline, t0, false, 0));
  CHECK_FALSE(deadline.started);

  CHECK_FALSE(disconnectDeadlineReached(&deadline, t0, false, 60));
  CHECK(deadline.started);
  CHECK(deadline.since == t0);
  CHECK_FALSE(disconnectDeadlineReached(
      &deadline, t0 + std::chrono::seconds(59), false, 60));
  CHECK(disconnectDeadlineReached(&deadline, t0 + std::chrono::seconds(60),
                                  false, 60));

  CHECK_FALSE(disconnectDeadlineReached(
      &deadline, t0 + std::chrono::seconds(1000), true, 60));
  CHECK_FALSE(deadline.started);
  CHECK_FALSE(disconnectDeadlineReached(
      &deadline, t0 + std::chrono::seconds(1000), false, 60));
  CHECK(deadline.started);
  CHECK_FALSE(disconnectDeadlineReached(nullptr, t0, false, 60));
}

TEST_CASE_METHOD(TerminalSessionFixture,
                 "Disconnect timeout closes the terminal session",
                 "[TerminalServerLifecycle]") {
  server->setDisconnectTimeoutSeconds(1);
  startSession();
  waitForInit();
  CHECK(fcntl(terminalFd, F_GETFD) >= 0);

  REQUIRE(peerHandler->waitForData(terminalPeer, 4, 0));
  char packetType = 0;
  REQUIRE(peerHandler->read(terminalPeer, &packetType, 1) == 1);
  CHECK(packetType == TERMINAL_CLOSE);

  waitForSessionEnd();
  checkSessionClosed();
}

TEST_CASE("sessionDisconnectTimeoutSec prefers client InitialPayload",
          "[TerminalServerLifecycle]") {
  InitialPayload payload;
  CHECK(sessionDisconnectTimeoutSec(3600, payload) == 3600);
  payload.set_disconnect_timeout_seconds(1);
  CHECK(sessionDisconnectTimeoutSec(0, payload) == 1);
  CHECK(sessionDisconnectTimeoutSec(3600, payload) == 1);
  payload.set_disconnect_timeout_seconds(0);
  CHECK(sessionDisconnectTimeoutSec(3600, payload) == 0);
}

TEST_CASE_METHOD(TerminalSessionFixture,
                 "Client InitialPayload disconnect timeout closes the session",
                 "[TerminalServerLifecycle]") {
  // Server default remains 0 (no timeout). Only the client-requested
  // per-session deadline must close this idle disconnect — the path et1
  // relies on when it passes --disconnect-timeout to et.
  CHECK(server->getDisconnectTimeoutSeconds() == 0);
  client->payload.set_disconnect_timeout_seconds(1);
  startSession();
  waitForInit();
  CHECK(fcntl(terminalFd, F_GETFD) >= 0);

  REQUIRE(peerHandler->waitForData(terminalPeer, 4, 0));
  char packetType = 0;
  REQUIRE(peerHandler->read(terminalPeer, &packetType, 1) == 1);
  CHECK(packetType == TERMINAL_CLOSE);

  waitForSessionEnd();
  checkSessionClosed();
}

TEST_CASE_METHOD(TerminalSessionFixture,
                 "Client disconnect timeout survives pty-active resume",
                 "[TerminalServerLifecycle]") {
  // etserver restart (or any pty-active re-register) builds a fresh pump via
  // resumeClient with no client InitialPayload. The per-session timeout must
  // still come from TerminalUserInfo that etterminal preserved from TermInit.
  CHECK(server->getDisconnectTimeoutSeconds() == 0);
  server->dropConnection(id);
  closeTerminal();

  terminalPeer = peerHandler->connect(routerEndpoint);
  REQUIRE(terminalPeer >= 0);
  TerminalUserInfo userInfo;
  userInfo.set_id(id);
  userInfo.set_passkey(key);
  userInfo.set_uid(getuid());
  userInfo.set_gid(getgid());
  userInfo.set_ptyactive(true);
  userInfo.set_disconnect_timeout_seconds(1);
  peerHandler->writePacket(
      terminalPeer,
      Packet(TerminalPacketType::TERMINAL_USER_INFO, protoToString(userInfo)));
  REQUIRE(server->terminalRouter->acceptNewConnection().id == id);
  server->registerClient(client, key);
  auto info = server->terminalRouter->tryGetInfoForConnection(client);
  REQUIRE(info.has_value());
  terminalFd = info->fd();

  server->resumeClient(client);
  CHECK(fcntl(terminalFd, F_GETFD) >= 0);

  REQUIRE(peerHandler->waitForData(terminalPeer, 4, 0));
  char packetType = 0;
  REQUIRE(peerHandler->read(terminalPeer, &packetType, 1) == 1);
  CHECK(packetType == TERMINAL_CLOSE);

  waitForSessionEnd();
  checkSessionClosed();
}

TEST_CASE_METHOD(TerminalSessionFixture,
                 "Disconnect timeout closes a resumed terminal whose client "
                 "never returns",
                 "[TerminalServerLifecycle]") {
  reregisterWithoutClient();
  server->setRecoveryGraceSeconds(0);
  server->setDisconnectTimeoutSeconds(1);
  const auto t0 =
      std::chrono::steady_clock::time_point(std::chrono::seconds(1000));
  server->trackUnclaimedResume(id, t0);
  server->expireUnclaimedResumes(t0 + std::chrono::milliseconds(999));
  CHECK(server->clientKeyExists(id));
  CHECK(fcntl(terminalFd, F_GETFD) >= 0);

  server->expireUnclaimedResumes(t0 + std::chrono::seconds(1));
  CHECK(terminalReceivedClose());
  checkSessionClosed();
  CHECK_FALSE(server->terminalRouter->isPtyActive(id));
}

TEST_CASE_METHOD(TerminalSessionFixture,
                 "Recovery grace outlasts a shorter disconnect timeout",
                 "[TerminalServerLifecycle]") {
  reregisterWithoutClient();
  server->setRecoveryGraceSeconds(60);
  server->setDisconnectTimeoutSeconds(1);
  const auto t0 =
      std::chrono::steady_clock::time_point(std::chrono::seconds(1000));
  server->trackUnclaimedResume(id, t0);
  server->expireUnclaimedResumes(t0 + std::chrono::seconds(59));
  CHECK(server->clientKeyExists(id));
  CHECK(fcntl(terminalFd, F_GETFD) >= 0);

  server->expireUnclaimedResumes(t0 + std::chrono::seconds(60));
  CHECK(terminalReceivedClose());
  checkSessionClosed();
}

TEST_CASE_METHOD(TerminalSessionFixture,
                 "A returning client takes over the resumed terminal deadline",
                 "[TerminalServerLifecycle]") {
  reregisterWithoutClient();
  server->setRecoveryGraceSeconds(0);
  server->setDisconnectTimeoutSeconds(1);
  const auto t0 =
      std::chrono::steady_clock::time_point(std::chrono::seconds(1000));
  server->trackUnclaimedResume(id, t0);
  server->registerClient(client, key);
  server->expireUnclaimedResumes(t0 + std::chrono::hours(1));
  CHECK(server->clientKeyExists(id));
  CHECK(fcntl(terminalFd, F_GETFD) >= 0);
  server->dropConnection(id);
  server->expireUnclaimedResumes(t0 + std::chrono::hours(2));
  CHECK(server->clientKeyExists(id));
}

TEST_CASE_METHOD(TerminalSessionFixture,
                 "Disconnected sessions retain their router until terminal EOF",
                 "[TerminalServerLifecycle]") {
  auto output = client->terminalOutput.get_future();
  startSession();
  waitForInit();
  REQUIRE(client->isDisconnected());
  const string text = "terminal still alive";
  TerminalBuffer tb;
  tb.set_buffer(text);
  peerHandler->writePacket(
      terminalPeer,
      Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
  REQUIRE(output.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready);
  CHECK(output.get() == text);
  CHECK(fcntl(terminalFd, F_GETFD) >= 0);
  CHECK(server->clientConnectionExists(id));
  closeTerminal();
  waitForSessionEnd();
  checkSessionClosed();
}
#endif
