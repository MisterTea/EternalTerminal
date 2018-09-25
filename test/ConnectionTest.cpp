#include "Headers.hpp"

#include "gtest/gtest.h"

#include "ClientConnection.hpp"
#include "Connection.hpp"
#include "LogHandler.hpp"
#include "PipeSocketHandler.hpp"
#include "ServerConnection.hpp"

using namespace et;

class Collector {
 public:
  Collector(shared_ptr<Connection> _connection) : connection(_connection), done(false) {
    collectorThread = std::thread(&Collector::run, this);
  }

  ~Collector() {
    done=true;
    collectorThread.join();
  }

  void run() {
    while (!done) {
      {
        lock_guard<std::mutex> guard(collectorMutex);
        string s;
        int status = connection->readMessage(&s);
        if (status == 1) {
          fifo.push_back(s);
          if (s == string("DONE")) {
            break;
          }
        } else if (status < 0) {
          FATAL_FAIL(status);
        }
      }
      ::usleep(1000);
    }
  }

  void finish() {
    done = true;
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
  std::thread collectorThread;
  std::mutex collectorMutex;
  bool done;
};

void listenFn(int serverFd, shared_ptr<ServerConnection> serverConnection) {
  // Only works when there is 1:1 mapping between endpoint and fds.  Will fix in
  // future api
  while (true) {
    if (serverConnection->acceptNewConnection(serverFd)) {
      return;
    }
    ::usleep(10 * 1000);  // Sleep 10ms for client to connect
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
    srand(1);

    serverSocketHandler.reset(new PipeSocketHandler());
    clientSocketHandler.reset(new PipeSocketHandler());

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
    std::thread serverListenThread(listenFn, serverFd, serverConnection);

    // Wait for server to spin up
    ::usleep(100 * 1000);
    clientConnection.reset(new ClientConnection(clientSocketHandler, endpoint,
                                                CLIENT_ID, CRYPTO_KEY));
    clientConnection->connect();
    serverListenThread.join();

    serverCollector.reset(
        new Collector(std::static_pointer_cast<Connection>(serverClientConnection)));
    clientCollector.reset(
        new Collector(std::static_pointer_cast<Connection>(clientConnection)));
  }

  void TearDown() override {
    FATAL_FAIL(::remove(pipePath.c_str()));
    FATAL_FAIL(::remove(pipeDirectory.c_str()));
  }

  shared_ptr<PipeSocketHandler> serverSocketHandler;
  shared_ptr<PipeSocketHandler> clientSocketHandler;
  shared_ptr<ServerConnection> serverConnection;
  shared_ptr<ClientConnection> clientConnection;
  shared_ptr<Collector> serverCollector;
  shared_ptr<Collector> clientCollector;
  string pipeDirectory;
  string pipePath;
};

TEST_F(ConnectionTest, ReadWrite) {
  string s(64 * 1024, '\0');
  for (int a = 0; a < 64 * 1024 - 1; a++) {
    s[a] = rand() % 26 + 'A';
  }
  s[64 * 1024 - 1] = 0;

  for (int a = 0; a < 64; a++) {
    VLOG(1) << "Writing packet " << a;
    serverCollector->write(string((&s[0] + a * 1024), 1024));
  }
  serverCollector->write("DONE");

  string resultConcat;
  string result;
  for (int a = 0; a < 64; a++) {
    result = clientCollector->read();
    resultConcat = resultConcat.append(result);
  }
  result = clientCollector->read();
  EXPECT_EQ(result, "DONE");

  serverCollector->finish();
  serverConnection->close();

  EXPECT_EQ(resultConcat, s);
}
