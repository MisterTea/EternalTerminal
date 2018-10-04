#include "Headers.hpp"

#include "gtest/gtest.h"

#include "ClientConnection.hpp"
#include "Connection.hpp"
#include "FlakySocketHandler.hpp"
#include "LogHandler.hpp"
#include "PipeSocketHandler.hpp"
#include "ServerConnection.hpp"

using namespace et;

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
        string s;
        int status = connection->readMessage(&s);
        if (status == 1) {
          if (s == string("DONE")) {
            fifo.push_back(s);
            break;
          }
          if (s != string("HEARTBEAT")) {
            fifo.push_back(s);
          }
        } else if (status < 0) {
          FATAL_FAIL(status);
        }
      }
      ::usleep(1000);
      if (lastSecond <= time(NULL) - 5) {
        lock_guard<std::mutex> guard(collectorMutex);
        lastSecond = time(NULL);
        connection->writeMessage("HEARTBEAT");
      }
    }
  }

  void finish() {
    lock_guard<std::mutex> guard(collectorMutex);
    done = true;
    collectorThread->join();
    connection->shutdown();
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
      ::usleep(1000);
    }
    return pop();
  }

  void write(const string& s) { return connection->writeMessage(s); }

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
  // Only works when there is 1:1 mapping between endpoint and fds.  Will fix in
  // future api
  while (*stopListening == false) {
    if (serverConnection->getSocketHandler()->hasData(serverFd)) {
      serverConnection->acceptNewConnection(serverFd);
    }
    ::usleep(1000 * 1000);
  }
}

shared_ptr<ServerClientConnection> serverClientConnection;
class NewConnectionHandler : public ServerConnectionHandler {
 public:
  virtual bool newClient(
      shared_ptr<ServerClientConnection> _serverClientState) {
    serverClientConnection = _serverClientState;
    return true;
  }
};

class ConnectionTest : public testing::Test {
 protected:
  void SetUp() override {
    el::Helpers::setThreadName("Main");
    const string CRYPTO_KEY = "12345678901234567890123456789012";
    const string CLIENT_ID = "1234567890123456";

    string tmpPath = string("/tmp/et_test_XXXXXXXX");
    pipeDirectory = string(mkdtemp(&tmpPath[0]));
    pipePath = string(pipeDirectory) + "/pipe";
    SocketEndpoint endpoint(pipePath);

    serverConnection.reset(new ServerConnection(
        serverSocketHandler, endpoint,
        shared_ptr<ServerConnectionHandler>(new NewConnectionHandler())));
    serverConnection->addClientKey(CLIENT_ID, CRYPTO_KEY);

    int serverFd = *(serverSocketHandler->getEndpointFds(endpoint).begin());
    stopListening = false;
    serverListenThread.reset(
        new std::thread(listenFn, &stopListening, serverFd, serverConnection));

    // Wait for server to spin up
    ::usleep(1000 * 1000);
    clientConnection.reset(new ClientConnection(clientSocketHandler, endpoint,
                                                CLIENT_ID, CRYPTO_KEY));
    while (true) {
      try {
        clientConnection->connect();
        break;
      } catch (const std::runtime_error& ex) {
        LOG(INFO) << "Connection failed, retrying...";
        ::usleep(10 * 1000);
      }
    }

    ::usleep(1000 * 1000);
    if(serverClientConnection.get() == NULL) {
      LOG(FATAL) << "Missing server connection...";
    }
    serverCollector.reset(new Collector(
        std::static_pointer_cast<Connection>(serverClientConnection), "Server"));
    serverCollector->start();
    clientCollector.reset(
        new Collector(std::static_pointer_cast<Connection>(clientConnection), "Client"));
    clientCollector->start();
  }

  void TearDown() override {
    FATAL_FAIL(::remove(pipePath.c_str()));
    FATAL_FAIL(::remove(pipeDirectory.c_str()));
  }

  void readWriteTest() {
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
    EXPECT_EQ(result, "DONE");

    clientConnection->shutdown();
    serverConnection->shutdown();
    stopListening = true;
    serverListenThread->join();
    serverCollector->finish();
    clientCollector->finish();

    EXPECT_EQ(resultConcat, s);
  }

  shared_ptr<SocketHandler> serverSocketHandler;
  shared_ptr<SocketHandler> clientSocketHandler;
  shared_ptr<ServerConnection> serverConnection;
  shared_ptr<ClientConnection> clientConnection;
  shared_ptr<Collector> serverCollector;
  shared_ptr<Collector> clientCollector;
  shared_ptr<std::thread> serverListenThread;
  string pipeDirectory;
  string pipePath;
  bool stopListening;
};

class ReliableConnectionTest : public ConnectionTest {
 protected:
  void SetUp() override {
    srand(1);

    serverSocketHandler.reset(new PipeSocketHandler());
    clientSocketHandler.reset(new PipeSocketHandler());

    ConnectionTest::SetUp();
  }
};

TEST_F(ReliableConnectionTest, ReadWrite) { readWriteTest(); }

class FlakyConnectionTest : public ConnectionTest {
 protected:
  void SetUp() override {
    int seed = int(time(NULL));
    srand(seed);
    LOG(INFO) << "Running flaky test with seed: " << seed;

    shared_ptr<SocketHandler> serverReliableSocketHandler(
        new PipeSocketHandler());
    shared_ptr<SocketHandler> clientReliableSocketHandler(
        new PipeSocketHandler());
    serverSocketHandler.reset(
        new FlakySocketHandler(serverReliableSocketHandler));
    clientSocketHandler.reset(
        new FlakySocketHandler(clientReliableSocketHandler));

    ConnectionTest::SetUp();
  }
};

TEST_F(FlakyConnectionTest, ReadWrite) { readWriteTest(); }
