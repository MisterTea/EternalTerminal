#ifndef WIN32

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "BackedWriter.hpp"
#include "FakeConsole.hpp"
#include "FakeSshSetupHandler.hpp"
#include "PipeSocketHandler.hpp"
#include "TerminalClient.hpp"
#include "TerminalServer.hpp"
#include "TestHeaders.hpp"
#include "UserTerminalHandler.hpp"

using namespace et;

namespace {

// Reconnect replays whatever BackedWriter still has. The header byte is in
// the clear, so this sees a destination request without decrypting it.
bool recoveryHasDestinationRequest(const shared_ptr<BackedWriter>& writer,
                                   int64_t afterSequence) {
  for (const auto& serialized : writer->recover(afterSequence)) {
    if (serialized.size() >= static_cast<size_t>(Packet::HEADER_SIZE) &&
        static_cast<uint8_t>(serialized[1]) ==
            static_cast<uint8_t>(
                TerminalPacketType::PORT_FORWARD_DESTINATION_REQUEST)) {
      return true;
    }
  }
  return false;
}

int64_t writerSequence(const shared_ptr<BackedWriter>& writer) {
  lock_guard<std::mutex> guard(writer->getRecoverMutex());
  return writer->getSequenceNumber();
}

// Holds one forwarded connection open so the accepting side keeps it parked.
class Dial {
 public:
  Dial() = default;

  bool connectUntil(const string& path, std::chrono::milliseconds budget) {
    endpoint_.set_name(path);
    const auto deadline = std::chrono::steady_clock::now() + budget;
    do {
      fd_ = handler_->connect(endpoint_);
      if (fd_ >= 0) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  ~Dial() {
    if (fd_ >= 0) {
      handler_->close(fd_);
    }
  }

  Dial(const Dial&) = delete;
  Dial& operator=(const Dial&) = delete;

 private:
  shared_ptr<PipeSocketHandler> handler_ = make_shared<PipeSocketHandler>();
  SocketEndpoint endpoint_;
  int fd_ = -1;
};

bool waitForSequencePast(const shared_ptr<BackedWriter>& writer, int64_t before,
                         std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (writerSequence(writer) == before &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return writerSequence(writer) > before;
}

enum class ClientLoop { Run, Idle };

// The client's network socket. Reconnect goes through connect(); forwards do
// not. Refusing connect keeps a severed client disconnected.
class GatedSocketHandler : public PipeSocketHandler {
 public:
  atomic<bool> allowConnect{true};

  int connect(const SocketEndpoint& endpoint) override {
    if (!allowConnect.load()) {
      SetErrno(ECONNREFUSED);
      return -1;
    }
    return PipeSocketHandler::connect(endpoint);
  }
};

struct ReplayClient : public TerminalClient {
  using TerminalClient::TerminalClient;

  void sever() { connection->closeSocket(); }
  bool disconnected() { return connection->isDisconnected(); }
  shared_ptr<BackedWriter> writer() { return connection->getWriter(); }
};

// Real client and server loops. A local forward is accepted by the client
// loop under test. A reverse forward is accepted by
// TerminalServer::runTerminal.
struct ReplaySession {
  string pipeDirectory;
  string localSource;
  string reverseSource;
  shared_ptr<PipeSocketHandler> serverSocketHandler;
  shared_ptr<PipeSocketHandler> routerSocketHandler;
  shared_ptr<GatedSocketHandler> clientSocketHandler;
  shared_ptr<PipeSocketHandler> clientPipeSocketHandler;
  shared_ptr<FakeUserTerminal> fakeUserTerminal;
  shared_ptr<TerminalServer> server;
  shared_ptr<UserTerminalHandler> uth;
  shared_ptr<ReplayClient> client;
  shared_ptr<FakeConsole> console;
  thread serverThread;
  thread uthThread;
  thread clientThread;
  string id;
  atomic<bool> keepIdle{true};
  atomic<bool> clientLoopRunning{false};

  explicit ReplaySession(ClientLoop loop) {
    try {
      start(loop);
    } catch (...) {
      // A throw skips ~ReplaySession, and a still-joinable std::thread aborts.
      stop();
      throw;
    }
  }

  void start(ClientLoop loop) {
    serverSocketHandler = make_shared<PipeSocketHandler>();
    routerSocketHandler = make_shared<PipeSocketHandler>();
    clientSocketHandler = make_shared<GatedSocketHandler>();
    clientPipeSocketHandler = make_shared<PipeSocketHandler>();

    pipeDirectory = test::makeTempDir("etfr");
    localSource = pipeDirectory + "/ls";
    reverseSource = pipeDirectory + "/rs";
    const string localDest = pipeDirectory + "/ld";
    const string reverseDest = pipeDirectory + "/rd";

    SocketEndpoint routerEndpoint;
    routerEndpoint.set_name(pipeDirectory + "/router");
    SocketEndpoint serverEndpoint;
    serverEndpoint.set_name(pipeDirectory + "/server");

    server = make_shared<TerminalServer>(serverSocketHandler, serverEndpoint,
                                         routerSocketHandler, routerEndpoint);
    serverThread = thread([this]() { server->run(); });
    testSleepMicros(200000);

    auto sshSetupHandler =
        make_shared<FakeSshSetupHandler>(make_shared<FakeSubprocessUtils>());
    auto idpass = sshSetupHandler->SetupSsh("", "localhost", "localhost", 2022,
                                            "", "", false, 0, "", "", {});
    id = idpass.first;

    fakeUserTerminal = make_shared<FakeUserTerminal>(routerSocketHandler);
    uth = make_shared<UserTerminalHandler>(
        routerSocketHandler, fakeUserTerminal, true, routerEndpoint,
        id + "/" + idpass.second);
    uthThread = thread([this]() { uth->run(); });
    // setup() runs only after the server sends TERMINAL_INIT, which happens
    // once the client below connects. Waiting here would deadlock.

    if (loop == ClientLoop::Run) {
      console = make_shared<FakeConsole>(make_shared<PipeSocketHandler>());
      TerminalInfo info;
      info.set_row(24);
      info.set_column(80);
      console->setTerminalInfoResult(info);
    }

    client = make_shared<ReplayClient>(
        clientSocketHandler, clientPipeSocketHandler, serverEndpoint, id,
        idpass.second, console, false, localSource + ":" + localDest,
        reverseSource + ":" + reverseDest, false, "",
        MAX_CLIENT_KEEP_ALIVE_DURATION, vector<pair<string, string>>{});

    clientLoopRunning = true;
    if (loop == ClientLoop::Run) {
      clientThread = thread([this]() {
        client->run("", false);
        clientLoopRunning = false;
      });
    } else {
      clientThread = thread([this]() {
        client->serviceIdleUntil([this]() { return keepIdle.load(); });
        clientLoopRunning = false;
      });
    }
    testSleepMicros(200000);
    REQUIRE(clientLoopRunning);
  }

  ~ReplaySession() { stop(); }

  shared_ptr<ServerClientConnection> serverConnection() {
    return server->getClientConnection(id);
  }

  void sever() {
    // A read error on the old socket starts reconnect. Refuse it so the
    // forward loops stay up and see a disconnected peer.
    clientSocketHandler->allowConnect = false;
    client->sever();
    auto serverConn = server->tryGetClientConnection(id);
    if (serverConn) {
      serverConn->closeSocket();
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline &&
           (!client->disconnected() ||
            (serverConn && !serverConn->isDisconnected()))) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  // Stop the loops without destroying the connections, so their writers can
  // still be read.
  void joinLoops() {
    keepIdle = false;
    if (client) {
      client->shutdown();
    }
    if (clientThread.joinable()) {
      clientThread.join();
    }
    clientLoopRunning = false;
    if (server) {
      server->shutdown();
    }
    if (serverThread.joinable()) {
      serverThread.join();
    }
  }

  void stop() {
    joinLoops();
    if (uth) {
      uth->shutdown();
    }
    if (uthThread.joinable()) {
      uthThread.join();
    }
    uth.reset();
    if (server) {
      if (auto conn = server->tryGetClientConnection(id)) {
        conn->shutdown();
      }
    }
    server.reset();
    client.reset();
    fakeUserTerminal.reset();
    if (!pipeDirectory.empty()) {
      std::error_code ec;
      fs::remove_all(pipeDirectory, ec);
      pipeDirectory.clear();
    }
  }
};

shared_ptr<Dial> dial(const string& path) {
  auto opened = make_shared<Dial>();
  REQUIRE(opened->connectUntil(path, std::chrono::seconds(2)));
  return opened;
}

}  // namespace

// Handler tests never call writePacket, and peerConnected defaults to true,
// so a run loop that drops the flag still leaves those tests green. These
// drive the loops and read the buffer reconnect would replay.
TEST_CASE("Idle and server loops do not replay forwards accepted while down",
          "[PortForward][Replay]") {
  ReplaySession session(ClientLoop::Idle);
  vector<shared_ptr<Dial>> open;

  const int64_t clientBefore = writerSequence(session.client->writer());
  open.push_back(dial(session.localSource));
  REQUIRE(waitForSequencePast(session.client->writer(), clientBefore,
                              std::chrono::seconds(2)));

  const int64_t serverBefore =
      writerSequence(session.serverConnection()->getWriter());
  open.push_back(dial(session.reverseSource));
  REQUIRE(waitForSequencePast(session.serverConnection()->getWriter(),
                              serverBefore, std::chrono::seconds(2)));

  session.sever();
  REQUIRE(session.client->disconnected());
  REQUIRE(session.serverConnection()->isDisconnected());
  REQUIRE(
      recoveryHasDestinationRequest(session.client->writer(), clientBefore));
  REQUIRE(recoveryHasDestinationRequest(session.serverConnection()->getWriter(),
                                        serverBefore));

  const int64_t clientParked = writerSequence(session.client->writer());
  const int64_t serverParked =
      writerSequence(session.serverConnection()->getWriter());
  open.push_back(dial(session.localSource));
  open.push_back(dial(session.reverseSource));
  // Several poll iterations, so an unfixed loop would have written by now.
  testSleepMicros(1000000);
  REQUIRE(session.clientLoopRunning);

  session.joinLoops();
  REQUIRE_FALSE(
      recoveryHasDestinationRequest(session.client->writer(), clientParked));
  REQUIRE_FALSE(recoveryHasDestinationRequest(
      session.serverConnection()->getWriter(), serverParked));
}

TEST_CASE("TerminalClient run does not replay forwards accepted while down",
          "[PortForward][Replay]") {
  ReplaySession session(ClientLoop::Run);
  vector<shared_ptr<Dial>> open;

  const int64_t clientBefore = writerSequence(session.client->writer());
  open.push_back(dial(session.localSource));
  REQUIRE(waitForSequencePast(session.client->writer(), clientBefore,
                              std::chrono::seconds(2)));

  session.sever();
  REQUIRE(session.client->disconnected());
  REQUIRE(
      recoveryHasDestinationRequest(session.client->writer(), clientBefore));

  const int64_t clientParked = writerSequence(session.client->writer());
  open.push_back(dial(session.localSource));
  testSleepMicros(1000000);
  REQUIRE(session.clientLoopRunning);

  session.joinLoops();
  REQUIRE_FALSE(
      recoveryHasDestinationRequest(session.client->writer(), clientParked));
}

#endif
