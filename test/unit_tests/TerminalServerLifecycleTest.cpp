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

  void registerTerminal() {
    terminalPeer = peerHandler->connect(routerEndpoint);
    REQUIRE(terminalPeer >= 0);
    TerminalUserInfo userInfo;
    userInfo.set_id(id);
    userInfo.set_passkey(key);
    userInfo.set_uid(getuid());
    userInfo.set_gid(getgid());
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

TEST_CASE_METHOD(TerminalSessionFixture,
                 "Disconnected sessions retain their router until terminal EOF",
                 "[TerminalServerLifecycle]") {
  auto output = client->terminalOutput.get_future();
  startSession();
  waitForInit();
  REQUIRE(client->isDisconnected());
  const string text = "terminal still alive";
  REQUIRE(peerHandler->write(terminalPeer, text.data(), text.size()) ==
          static_cast<ssize_t>(text.size()));
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
