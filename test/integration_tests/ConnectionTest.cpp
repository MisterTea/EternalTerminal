#include <atomic>
#include <chrono>

#include "ClientConnection.hpp"
#include "Connection.hpp"
#include "FlakySocketHandler.hpp"
#include "LogHandler.hpp"
#include "PipeSocketHandler.hpp"
#include "ServerConnection.hpp"
#include "TestHeaders.hpp"

using namespace et;

const int HEADER_DONE = 0;
const int HEADER_DATA = 1;
const string CRYPTO_KEY = "12345678901234567890123456789012";

class Collector {
 public:
  Collector(shared_ptr<Connection> _connection, const string& _threadName)
      : connection(_connection), threadName(_threadName), done(false) {}

  ~Collector() {
    if (done == false) {
      STFATAL << "Did not shut down properly";
    }
  }

  void start() {
    collectorThread.reset(new std::thread(&Collector::run, this));
  }

  void run() {
    el::Helpers::setThreadName(threadName);
    auto lastSecond = time(NULL);
    while (true) {
      {
        lock_guard<std::recursive_mutex> guard(collectorMutex);
        if (done) {
          break;
        }
      }
      if (connection.get() == NULL) {
        STFATAL << "CONNECTION IS NULL";
      }
      if (connection->hasData()) {
        Packet packet;
        bool status = connection->readPacket(&packet);
        lock_guard<std::recursive_mutex> guard(collectorMutex);
        if (status) {
          if (packet.getHeader() == HEADER_DONE) {
            fifo.push_back("DONE");
          } else if (packet.getHeader() == HEADER_DATA) {
            fifo.push_back(packet.getPayload());
          } else if (packet.getHeader() == HEARTBEAT) {
            // Do nothing
          } else {
            STFATAL << "INVALID PACKET HEADER: " << packet.getHeader();
          }
        }
      }
      if (connection->isShuttingDown()) {
        lock_guard<std::recursive_mutex> guard(collectorMutex);
        done = true;
      }
      testSleepMicros(10 * 1000);
      if (lastSecond <= time(NULL) - 5) {
        lock_guard<std::recursive_mutex> guard(collectorMutex);
        lastSecond = time(NULL);
        connection->writePacket(Packet(EtPacketType::HEARTBEAT, ""));
      }
    }
  }

  void join() { collectorThread->join(); }

  void finish() {
    connection->shutdown();
    {
      lock_guard<std::recursive_mutex> guard(collectorMutex);
      done = true;
    }
    collectorThread->join();
  }

  bool hasData() {
    lock_guard<std::recursive_mutex> guard(collectorMutex);
    return !fifo.empty();
  }

  string pop() {
    lock_guard<std::recursive_mutex> guard(collectorMutex);
    if (fifo.empty()) {
      STFATAL << "Tried to pop an empty fifo";
    }
    string s = fifo.front();
    fifo.pop_front();
    return s;
  }

  string read() {
    while (!hasData()) {
      testSleepMicros(10 * 1000);
    }
    return pop();
  }

  void write(const string& s) {
    return connection->writePacket(Packet(HEADER_DATA, s));
  }

  shared_ptr<Connection> getConnection() { return connection; }

 protected:
  shared_ptr<Connection> connection;
  deque<string> fifo;
  shared_ptr<std::thread> collectorThread;
  std::recursive_mutex collectorMutex;
  string threadName;
  bool done;
};

recursive_mutex testMutex;
void listenFn(bool* stopListening, int serverFd,
              shared_ptr<ServerConnection> serverConnection) {
  while (true) {
    {
      lock_guard<recursive_mutex> lock(testMutex);
      if (*stopListening) {
        break;
      }
    }
    if (serverConnection->getSocketHandler()->hasData(serverFd)) {
      serverConnection->acceptNewConnection(serverFd);
    }
    testSleepMicros(10 * 1000);
  }
}

map<string, shared_ptr<ServerClientConnection>> serverClientConnections;
mutex serverClientConnectionMutex;
class TestServerConnection : public ServerConnection {
 public:
  TestServerConnection(shared_ptr<SocketHandler> _socketHandler,
                       SocketEndpoint socketEndpoint)
      : ServerConnection(_socketHandler, socketEndpoint) {}
  virtual ~TestServerConnection() {}
  virtual bool newClient(
      shared_ptr<ServerClientConnection> _serverClientState) {
    string clientId = _serverClientState->getId();
    lock_guard<mutex> lock(serverClientConnectionMutex);
    if (serverClientConnections.find(clientId) !=
        serverClientConnections.end()) {
      STFATAL << "TRIED TO CREATE DUPLICATE CLIENT ID";
    }
    serverClientConnections[clientId] = _serverClientState;
    return true;
  }
};

void readWriteTest(const string& clientId,
                   shared_ptr<SocketHandler> clientSocketHandler,
                   shared_ptr<ServerConnection> serverConnection,
                   SocketEndpoint endpoint) {
  serverConnection->addClientKey(clientId, CRYPTO_KEY);
  // Wait for server to spin up
  testSleepMicros(1000 * 1000);

  shared_ptr<ClientConnection> clientConnection(new ClientConnection(
      clientSocketHandler, endpoint, clientId, CRYPTO_KEY));
  while (true) {
    try {
      if (clientConnection->connect()) {
        break;
      }
      LOG(INFO) << "Connection failed, retrying...";
      testSleepMicros(1000 * 1000);
    } catch (const std::runtime_error& ex) {
      STFATAL << "Error connecting to server: " << ex.what();
    }
  }

  while (true) {
    {
      lock_guard<mutex> lock(serverClientConnectionMutex);
      if (serverClientConnections.find(clientId) !=
          serverClientConnections.end()) {
        break;
      }
    }
    testSleepMicros(1000 * 1000);
  }
  shared_ptr<ServerClientConnection> serverClientConnection;
  {
    lock_guard<mutex> lock(serverClientConnectionMutex);
    serverClientConnection = serverClientConnections.find(clientId)->second;
  }
  shared_ptr<Collector> serverCollector(new Collector(
      std::static_pointer_cast<Connection>(serverClientConnection), "Server"));
  serverCollector->start();
  shared_ptr<Collector> clientCollector(new Collector(
      std::static_pointer_cast<Connection>(clientConnection), "Client"));
  clientCollector->start();

  const int NUM_MESSAGES = 32;
  string s(NUM_MESSAGES * 1024, '\0');
  for (int a = 0; a < NUM_MESSAGES * 1024; a++) {
    s[a] = rand() % 26 + 'A';
  }

  for (int a = 0; a < NUM_MESSAGES; a++) {
    VLOG(1) << "Writing packet " << a;
    serverCollector->write(string((&s[0] + a * 1024), 1024));
  }
  serverCollector->write("DONE");

  string resultConcat;
  string result;
  for (int a = 0; a < NUM_MESSAGES; a++) {
    result = clientCollector->read();
    resultConcat = resultConcat.append(result);
    LOG(INFO) << "ON MESSAGE " << a;
  }
  result = clientCollector->read();
  {
    lock_guard<recursive_mutex> lock(testMutex);
    REQUIRE(result == "DONE");
    REQUIRE(resultConcat == s);
  }

  clientCollector->finish();
  clientCollector.reset();
  clientConnection.reset();

  serverConnection->removeClient(serverCollector->getConnection()->getId());
  serverCollector->join();
  serverCollector.reset();
}

void multiReadWriteTest(shared_ptr<SocketHandler> clientSocketHandler,
                        shared_ptr<ServerConnection> serverConnection,
                        SocketEndpoint endpoint) {
  ThreadPool pool(16);
  string base_id = "1234567890123456";
  vector<future<void>> futures;
  for (int a = 0; a < 16; a++) {
    string new_id = base_id;
    new_id[0] = 'A' + a;
    auto f = pool.enqueue(
        [clientSocketHandler, serverConnection, endpoint](string clientId) {
          readWriteTest(clientId, clientSocketHandler, serverConnection,
                        endpoint);
        },
        new_id);
    futures.push_back(std::move(f));
    testSleepMicros((500 + rand() % 1000) * 1000);
  }
  for (auto& f : futures) {
    f.get();
  }
}

bool readWithDeadline(shared_ptr<Collector> collector, int seconds,
                      string* result) {
  time_t deadline = time(NULL) + seconds;
  while (time(NULL) <= deadline) {
    if (collector->hasData()) {
      *result = collector->pop();
      return true;
    }
    testSleepMicros(10 * 1000);
  }
  return false;
}

shared_ptr<ServerClientConnection> waitForServerClient(const string& clientId,
                                                       int seconds) {
  time_t deadline = time(NULL) + seconds;
  while (time(NULL) <= deadline) {
    {
      lock_guard<mutex> lock(serverClientConnectionMutex);
      auto it = serverClientConnections.find(clientId);
      if (it != serverClientConnections.end()) {
        return it->second;
      }
    }
    testSleepMicros(10 * 1000);
  }
  return nullptr;
}

// connect() overwrites socketFd without closing the previous one. Drop those
// leftovers so a failed assertion still reaches Catch instead of the harness
// aborting on a dangling fd.
void closeLeftoverClientSockets(shared_ptr<FlakySocketHandler> handler) {
  for (int fd : handler->getActiveSockets()) {
    handler->close(fd);
  }
}

bool waitForPeerClose(shared_ptr<SocketHandler> socketHandler, int fd) {
  time_t startTime = time(NULL);
  char byte;
  while (time(NULL) <= startTime + 5) {
    if (!socketHandler->hasData(fd)) {
      testSleepMicros(10 * 1000);
      continue;
    }

    ssize_t bytesRead = socketHandler->read(fd, &byte, sizeof(byte));
    if (bytesRead == 0) {
      return true;
    }
    if (bytesRead < 0) {
      auto localErrno = GetErrno();
      if (localErrno == EAGAIN || localErrno == EWOULDBLOCK) {
        continue;
      }
      return true;
    }
  }
  return false;
}

struct ConnectionTestContext {
  shared_ptr<FlakySocketHandler> serverSocketHandler;
  shared_ptr<FlakySocketHandler> clientSocketHandler;
  shared_ptr<ServerConnection> serverConnection;
  SocketEndpoint endpoint;
  string pipeDirectory;
  string pipePath;
  shared_ptr<std::thread> serverListenThread;
  int serverFd{-1};
  bool stopListening{false};
};

void runConnectionTestCase(bool flaky,
                           const function<void(ConnectionTestContext&)>& body) {
  srand(1);

  ConnectionTestContext ctx;
  ctx.serverSocketHandler.reset(new FlakySocketHandler(
      shared_ptr<SocketHandler>(new PipeSocketHandler()), false));
  ctx.clientSocketHandler.reset(new FlakySocketHandler(
      shared_ptr<SocketHandler>(new PipeSocketHandler()), false));
  if (flaky) {
    ctx.serverSocketHandler->setFlake(true);
    ctx.clientSocketHandler->setFlake(true);
  }

  el::Helpers::setThreadName("Main");

#ifdef WIN32
  ctx.pipePath = "et_connection_test_" + genRandomAlphaNum(12) + ".ipc";
#else
  string tmpPath = GetTempDirectory() + string("et_test_XXXXXXXX");
  ctx.pipeDirectory = string(mkdtemp(&tmpPath[0]));
  ctx.pipePath = string(ctx.pipeDirectory) + "/pipe";
#endif
  ctx.endpoint = SocketEndpoint();
  ctx.endpoint.set_name(ctx.pipePath);

  ctx.serverConnection.reset(
      new TestServerConnection(ctx.serverSocketHandler, ctx.endpoint));

  ctx.serverFd =
      *(ctx.serverSocketHandler->getEndpointFds(ctx.endpoint).begin());
  {
    lock_guard<recursive_mutex> lock(testMutex);
    ctx.stopListening = false;
  }

  ctx.serverListenThread.reset(new std::thread(
      listenFn, &ctx.stopListening, ctx.serverFd, ctx.serverConnection));

  body(ctx);

  {
    lock_guard<recursive_mutex> lock(testMutex);
    ctx.stopListening = true;
  }
  ctx.serverListenThread->join();
  ctx.serverListenThread.reset();
  serverClientConnections.clear();
  ctx.serverConnection->shutdown();
  ctx.serverConnection.reset();
  removeOrMissing(ctx.pipePath);
#ifndef WIN32
  FATAL_FAIL(::remove(ctx.pipeDirectory.c_str()));
#endif

  auto v = ctx.serverSocketHandler->getActiveSockets();
  if (!v.empty()) {
    STFATAL << "Dangling socket fd (first): " << v[0];
  }
  v = ctx.clientSocketHandler->getActiveSockets();
  if (!v.empty()) {
    STFATAL << "Dangling socket fd (first): " << v[0];
  }
}

TEST_CASE("ConnectionTest_ReadWrite", "[ConnectionTest][integration]") {
  runConnectionTestCase(false, [](ConnectionTestContext& ctx) {
    readWriteTest("1234567890123456", ctx.clientSocketHandler,
                  ctx.serverConnection, ctx.endpoint);
  });
}

TEST_CASE("ConnectionTest_ReadWrite_Flaky", "[ConnectionTest][integration]") {
  runConnectionTestCase(true, [](ConnectionTestContext& ctx) {
    readWriteTest("1234567890123456", ctx.clientSocketHandler,
                  ctx.serverConnection, ctx.endpoint);
  });
}

TEST_CASE("ConnectionTest_MultiReadWrite", "[ConnectionTest][integration]") {
  runConnectionTestCase(false, [](ConnectionTestContext& ctx) {
    multiReadWriteTest(ctx.clientSocketHandler, ctx.serverConnection,
                       ctx.endpoint);
  });
}

TEST_CASE("ConnectionTest_MultiReadWrite_Flaky",
          "[ConnectionTest][integration]") {
  runConnectionTestCase(true, [](ConnectionTestContext& ctx) {
    multiReadWriteTest(ctx.clientSocketHandler, ctx.serverConnection,
                       ctx.endpoint);
  });
}

TEST_CASE("ConnectionTest_InvalidClient", "[ConnectionTest][integration]") {
  runConnectionTestCase(false, [](ConnectionTestContext& ctx) {
    for (int a = 0; a < 128; a++) {
      int fd = ctx.clientSocketHandler->connect(ctx.endpoint);
      REQUIRE(fd >= 0);

      int64_t invalidLength =
          (a % 2 == 0) ? -1 : (int64_t(128) * 1024 * 1024) + 1;
      ctx.clientSocketHandler->writeAllOrThrow(fd, &invalidLength,
                                               sizeof(invalidLength), true);

      REQUIRE(waitForPeerClose(ctx.clientSocketHandler, fd));
      ctx.clientSocketHandler->close(fd);
    }

    lock_guard<mutex> lock(serverClientConnectionMutex);
    REQUIRE(serverClientConnections.empty());
  });
}

// https://github.com/MisterTea/EternalTerminal/issues/862
// A second connect() on the same ClientConnection is what TerminalClient would
// do if it retried after a slow INITIAL_RESPONSE. The server already has the
// session, answers RETURNING_CLIENT, and runs recoverClient(). connect()
// accepts that status but does not recover: it installs a fresh reader and
// writer at sequence 0. The packet written afterwards (the resent
// INITIAL_PAYLOAD) never arrives.
TEST_CASE("ConnectionTest_ConnectRetryAfterNewClient",
          "[ConnectionTest][integration]") {
  string clientReceived;
  string serverReceived;
  string failure;
  runConnectionTestCase(false, [&](ConnectionTestContext& ctx) {
    const string clientId = "1234567890123456";
    shared_ptr<ClientConnection> clientConnection;
    shared_ptr<Collector> serverCollector;
    shared_ptr<Collector> clientCollector;
    auto finish = [&]() {
      if (clientCollector) {
        clientCollector->finish();
        clientCollector.reset();
      }
      if (clientConnection) {
        clientConnection->shutdown();
        clientConnection->waitReconnect();
        clientConnection.reset();
      }
      ctx.serverConnection->removeClient(clientId);
      if (serverCollector) {
        serverCollector->join();
        serverCollector.reset();
      }
      closeLeftoverClientSockets(ctx.clientSocketHandler);
    };

    ctx.serverConnection->addClientKey(clientId, CRYPTO_KEY);
    clientConnection.reset(new ClientConnection(
        ctx.clientSocketHandler, ctx.endpoint, clientId, CRYPTO_KEY));
    if (!clientConnection->connect()) {
      failure = "initial connect() failed";
      finish();
      return;
    }
    clientConnection->writePacket(Packet(HEADER_DATA, "first"));

    shared_ptr<ServerClientConnection> serverClientConnection =
        waitForServerClient(clientId, 5);
    if (!serverClientConnection) {
      failure = "server did not accept the client";
      finish();
      return;
    }
    serverCollector.reset(new Collector(
        std::static_pointer_cast<Connection>(serverClientConnection),
        "Server"));
    serverCollector->start();
    string received;
    if (!readWithDeadline(serverCollector, 5, &received) ||
        received != "first") {
      failure = "server did not receive the first packet: '" + received + "'";
      finish();
      return;
    }

    // The retry, followed by the resend TerminalClient does after it.
    if (!clientConnection->connect()) {
      failure = "retry connect() failed";
      finish();
      return;
    }
    clientConnection->writePacket(Packet(HEADER_DATA, "second"));
    serverCollector->write("reply");

    clientCollector.reset(new Collector(
        std::static_pointer_cast<Connection>(clientConnection), "Client"));
    clientCollector->start();

    bool clientGotReply = readWithDeadline(clientCollector, 10, &received);
    clientReceived = clientGotReply ? received : "";
    bool serverGotSecond = readWithDeadline(serverCollector, 10, &received);
    serverReceived = serverGotSecond ? received : "";

    finish();
  });

  INFO("clientReceived='" << clientReceived << "' serverReceived='"
                          << serverReceived << "'");
  REQUIRE(failure.empty());
  REQUIRE(clientReceived == "reply");
  REQUIRE(serverReceived == "second");
}

// https://github.com/MisterTea/EternalTerminal/issues/861
// Connection::recover writes its whole CatchupBuffer before reading the
// peer's. When both directions are larger than the socket buffer, each side
// blocks in that write and recovery never finishes.
TEST_CASE("ConnectionTest_ReconnectLargeBidirectionalCatchup",
          "[ConnectionTest][integration]") {
  const string clientBlob(4 * 1024 * 1024, 'C');
  const string serverBlob(4 * 1024 * 1024, 'S');
  std::atomic<bool> clientDelivered{false};
  std::atomic<bool> serverDelivered{false};
  string clientGot;
  string serverGot;
  string failure;

  runConnectionTestCase(false, [&](ConnectionTestContext& ctx) {
    const string clientId = "1234567890123456";
    shared_ptr<ClientConnection> clientConnection;
    shared_ptr<ServerClientConnection> serverClientConnection;
    std::atomic<bool> stop{false};
    std::thread clientProbe;
    std::thread serverProbe;

    auto finish = [&]() {
      stop.store(true);
      if (clientConnection) {
        clientConnection->shutdown();
        clientConnection->waitReconnect();
      }
      ctx.serverConnection->removeClient(clientId);
      if (clientProbe.joinable()) {
        clientProbe.join();
      }
      if (serverProbe.joinable()) {
        serverProbe.join();
      }
      clientConnection.reset();
      serverClientConnection.reset();
      closeLeftoverClientSockets(ctx.clientSocketHandler);
    };

    ctx.serverConnection->addClientKey(clientId, CRYPTO_KEY);
    clientConnection.reset(new ClientConnection(
        ctx.clientSocketHandler, ctx.endpoint, clientId, CRYPTO_KEY));
    if (!clientConnection->connect()) {
      failure = "initial connect() failed";
      finish();
      return;
    }
    serverClientConnection = waitForServerClient(clientId, 5);
    if (!serverClientConnection) {
      failure = "server did not accept the client";
      finish();
      return;
    }

    // Drop both sockets without starting reconnect, then queue catchup that
    // cannot fit in an AF_UNIX socket buffer (~176 KiB on Linux).
    clientConnection->closeSocket();
    serverClientConnection->closeSocket();
    if (!clientConnection->write(Packet(HEADER_DATA, clientBlob))) {
      failure = "failed to buffer client catchup";
      finish();
      return;
    }
    if (!serverClientConnection->write(Packet(HEADER_DATA, serverBlob))) {
      failure = "failed to buffer server catchup";
      finish();
      return;
    }

    auto watch = [&](shared_ptr<Connection> connection, const string& expected,
                     std::atomic<bool>* delivered, string* got) {
      while (!stop.load()) {
        if (connection->hasData()) {
          Packet packet;
          if (connection->readPacket(&packet) &&
              packet.getHeader() == HEADER_DATA) {
            *got = packet.getPayload();
            delivered->store(*got == expected);
            return;
          }
        }
        testSleepMicros(10 * 1000);
      }
    };
    clientProbe = std::thread(
        watch, std::static_pointer_cast<Connection>(clientConnection),
        std::cref(serverBlob), &clientDelivered, &clientGot);
    serverProbe = std::thread(
        watch, std::static_pointer_cast<Connection>(serverClientConnection),
        std::cref(clientBlob), &serverDelivered, &serverGot);

    clientConnection->closeSocketAndMaybeReconnect();

    // A working recovery finishes in well under a second. The bug blocks in
    // writeProto until SOCKET_IDLE_TIMEOUT_SEC, so this wait returns first
    // and finish() then unblocks the stuck recover calls.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while ((!clientDelivered.load() || !serverDelivered.load()) &&
           std::chrono::steady_clock::now() < deadline) {
      testSleepMicros(20 * 1000);
    }
    finish();
  });

  INFO("failure=" << failure << " clientDelivered=" << clientDelivered.load()
                  << " clientBytes=" << clientGot.size()
                  << " serverDelivered=" << serverDelivered.load()
                  << " serverBytes=" << serverGot.size());
  REQUIRE(failure.empty());
  REQUIRE(clientDelivered.load());
  REQUIRE(serverDelivered.load());
}
