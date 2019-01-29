#include "TestHeaders.hpp"

#include "ClientConnection.hpp"
#include "Connection.hpp"
#include "FlakySocketHandler.hpp"
#include "LogHandler.hpp"
#include "PipeSocketHandler.hpp"
#include "ServerConnection.hpp"

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
      LOG(FATAL) << "Did not shut down properly";
    }
  }

  void start() {
    collectorThread.reset(new std::thread(&Collector::run, this));
  }

  void run() {
    el::Helpers::setThreadName(threadName);
    auto lastSecond = time(NULL);
    while (!done) {
      if (connection.get() == NULL) {
        LOG(FATAL) << "CONNECTION IS NULL";
      }
      if (connection->hasData()) {
        lock_guard<std::mutex> guard(collectorMutex);
        Packet packet;
        bool status = connection->readPacket(&packet);
        if (status) {
          if (packet.getHeader() == HEADER_DONE) {
            fifo.push_back("DONE");
          } else if (packet.getHeader() == HEADER_DATA) {
            fifo.push_back(packet.getPayload());
          } else if (packet.getHeader() == HEARTBEAT) {
            // Do nothing
          } else {
            LOG(FATAL) << "INVALID PACKET HEADER: " << packet.getHeader();
          }
        }
      }
      if (connection->isShuttingDown()) {
        done = true;
      }
      ::usleep(10 * 1000);
      if (lastSecond <= time(NULL) - 5) {
        lock_guard<std::mutex> guard(collectorMutex);
        lastSecond = time(NULL);
        connection->writePacket(Packet(EtPacketType::HEARTBEAT, ""));
      }
    }
  }

  void join() { collectorThread->join(); }

  void finish() {
    connection->shutdown();
    lock_guard<std::mutex> guard(collectorMutex);
    done = true;
    collectorThread->join();
  }

  bool hasData() {
    lock_guard<std::mutex> guard(collectorMutex);
    return !fifo.empty();
  }

  string pop() {
    lock_guard<std::mutex> guard(collectorMutex);
    if (fifo.empty()) {
      LOG(FATAL) << "Tried to pop an empty fifo";
    }
    string s = fifo.front();
    fifo.pop_front();
    return s;
  }

  string read() {
    while (!hasData()) {
      ::usleep(10 * 1000);
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
  std::mutex collectorMutex;
  string threadName;
  bool done;
};

void listenFn(bool* stopListening, int serverFd,
              shared_ptr<ServerConnection> serverConnection) {
  while (*stopListening == false) {
    if (serverConnection->getSocketHandler()->hasData(serverFd)) {
      serverConnection->acceptNewConnection(serverFd);
    }
    ::usleep(10 * 1000);
  }
}

map<string, shared_ptr<ServerClientConnection>> serverClientConnections;
class NewConnectionHandler : public ServerConnectionHandler {
 public:
  virtual bool newClient(
      shared_ptr<ServerClientConnection> _serverClientState) {
    string clientId = _serverClientState->getId();
    if (serverClientConnections.find(clientId) !=
        serverClientConnections.end()) {
      LOG(FATAL) << "TRIED TO CREATE DUPLICATE CLIENT ID";
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
  ::usleep(1000 * 1000);

  shared_ptr<ClientConnection> clientConnection(new ClientConnection(
      clientSocketHandler, endpoint, clientId, CRYPTO_KEY));
  while (true) {
    try {
      if (clientConnection->connect()) {
        break;
      }
      LOG(INFO) << "Connection failed, retrying...";
      ::usleep(1000 * 1000);
    } catch (const std::runtime_error& ex) {
      LOG(FATAL) << "Error connecting to server: " << ex.what();
    }
  }

  while (serverClientConnections.find(clientId) ==
         serverClientConnections.end()) {
    ::usleep(1000 * 1000);
  }
  shared_ptr<Collector> serverCollector(
      new Collector(std::static_pointer_cast<Connection>(
                        serverClientConnections.find(clientId)->second),
                    "Server"));
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
  REQUIRE(result == "DONE");
  REQUIRE(resultConcat == s);

  serverConnection->removeClient(serverCollector->getConnection()->getId());
  serverCollector->join();
  serverCollector.reset();
  clientCollector->join();
  clientCollector.reset();
  clientConnection.reset();
}

void multiReadWriteTest(shared_ptr<SocketHandler> clientSocketHandler,
                        shared_ptr<ServerConnection> serverConnection,
                        SocketEndpoint endpoint) {
  thread_pool pool(16);
  string base_id = "1234567890123456";
  for (int a = 0; a < 16; a++) {
    string new_id = base_id;
    new_id[0] = 'A' + a;
    pool.push(
        [clientSocketHandler, serverConnection, endpoint](int id,
                                                          string clientId) {
          readWriteTest(clientId, clientSocketHandler, serverConnection,
                        endpoint);
        },
        new_id);
    ::usleep((500 + rand() % 1000) * 1000);
  }
  pool.stop(true);
}

TEST_CASE("ConnectionTest", "[ConnectionTest]") {
  srand(1);

  shared_ptr<FlakySocketHandler> serverSocketHandler(new FlakySocketHandler(
      shared_ptr<SocketHandler>(new PipeSocketHandler()), false));
  shared_ptr<FlakySocketHandler> clientSocketHandler(new FlakySocketHandler(
      shared_ptr<SocketHandler>(new PipeSocketHandler()), false));

  shared_ptr<ServerConnection> serverConnection;
  shared_ptr<std::thread> serverListenThread;
  string pipeDirectory;
  string pipePath;
  SocketEndpoint endpoint;
  bool stopListening;

  el::Helpers::setThreadName("Main");

  string tmpPath = string("/tmp/et_test_XXXXXXXX");
  pipeDirectory = string(mkdtemp(&tmpPath[0]));
  pipePath = string(pipeDirectory) + "/pipe";
  endpoint = SocketEndpoint(pipePath);

  serverConnection.reset(new ServerConnection(
      serverSocketHandler, endpoint,
      shared_ptr<ServerConnectionHandler>(new NewConnectionHandler())));

  int serverFd = *(serverSocketHandler->getEndpointFds(endpoint).begin());
  stopListening = false;
  serverListenThread.reset(
      new std::thread(listenFn, &stopListening, serverFd, serverConnection));

  SECTION("ReadWrite") {
    SECTION("Not Flaky") {}
    SECTION("Flaky") {
      serverSocketHandler->setFlake(true);
      clientSocketHandler->setFlake(true);
    }
    readWriteTest("1234567890123456", clientSocketHandler, serverConnection,
                  endpoint);
  }

  SECTION("MultiReadWrite") {
    SECTION("Not Flaky") {}
    SECTION("Flaky") {
      serverSocketHandler->setFlake(true);
      clientSocketHandler->setFlake(true);
    }
    multiReadWriteTest(clientSocketHandler, serverConnection, endpoint);
  }

  SECTION("InvalidClient") {
    for (int a = 0; a < 128; a++) {
      string junk(16 * 1024 * 1024, 't');
      for (int b = 0; b < 16 * 1024 * 1024; b++) {
        junk[b] = rand() % 256;
      }
      int fd = clientSocketHandler->connect(endpoint);
      int retval =
          clientSocketHandler->writeAllOrReturn(fd, &junk[0], junk.length());
      REQUIRE(retval == -1);
      clientSocketHandler->close(fd);
    }
  }

  stopListening = true;
  serverListenThread->join();
  serverListenThread.reset();
  serverClientConnections.clear();
  serverConnection->shutdown();
  serverConnection.reset();
  FATAL_FAIL(::remove(pipePath.c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));

  auto v = serverSocketHandler->getActiveSockets();
  if (!v.empty()) {
    LOG(FATAL) << "Dangling socket fd (first): " << v[0];
  }
  v = clientSocketHandler->getActiveSockets();
  if (!v.empty()) {
    LOG(FATAL) << "Dangling socket fd (first): " << v[0];
  }
}
