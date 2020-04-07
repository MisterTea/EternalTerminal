#include "BackedReader.hpp"
#include "BackedWriter.hpp"
#include "CryptoHandler.hpp"
#include "FlakySocketHandler.hpp"
#include "LogHandler.hpp"
#include "PipeSocketHandler.hpp"
#include "TestHeaders.hpp"

using namespace et;

class BackedCollector {
 public:
  BackedCollector(shared_ptr<BackedReader> _reader,
                  shared_ptr<BackedWriter> _writer)
      : reader(_reader), writer(_writer), done(false) {
    collectorThread = std::thread(&BackedCollector::run, this);
  }

  ~BackedCollector() { finish(); }

  void run() {
    while (true) {
      {
        lock_guard<std::mutex> guard(collectorMutex);
        if (done) {
          break;
        }
      }
      Packet packet;
      if (reader->read(&packet) > 0) {
        lock_guard<std::mutex> guard(collectorMutex);
        fifo.push_back(packet.getPayload());
      } else {
        ::usleep(1000);
      }
    }
  }

  bool hasData() {
    lock_guard<std::mutex> guard(collectorMutex);
    return !fifo.empty();
  }

  string pop() {
    lock_guard<std::mutex> guard(collectorMutex);
    if (fifo.empty()) {
      STFATAL << "Tried to pop an empty fifo";
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

  void finish() {
    {
      lock_guard<std::mutex> guard(collectorMutex);
      done = true;
    }
    collectorThread.join();
  }

  BackedWriterWriteState write(string s) { return writer->write(Packet(0, s)); }

 protected:
  shared_ptr<BackedReader> reader;
  shared_ptr<BackedWriter> writer;
  deque<string> fifo;
  std::thread collectorThread;
  std::mutex collectorMutex;
  bool done;
};

void listenFn(shared_ptr<SocketHandler> socketHandler, SocketEndpoint endpoint,
              int* serverClientFd) {
  // Only works when there is 1:1 mapping between endpoint and fds.  Will fix in
  // future api
  int serverFd = *(socketHandler->listen(endpoint).begin());
  int fd;
  while (true) {
    fd = socketHandler->accept(serverFd);
    if (fd == -1) {
      if (errno != EAGAIN) {
        FATAL_FAIL(fd);
      } else {
        ::usleep(100 * 1000);  // Sleep for client to connect
      }
    } else {
      break;
    }
  }
  *serverClientFd = fd;
}

TEST_CASE("BackedTest", "[BackedTest]") {
  shared_ptr<SocketHandler> serverSocketHandler;
  shared_ptr<SocketHandler> clientSocketHandler;
  serverSocketHandler.reset(new PipeSocketHandler());
  clientSocketHandler.reset(new PipeSocketHandler());

  shared_ptr<BackedCollector> serverCollector;
  shared_ptr<BackedCollector> clientCollector;
  string pipeDirectory;
  string pipePath;

  string tmpPath = string("/tmp/et_test_XXXXXXXX");
  pipeDirectory = string(mkdtemp(&tmpPath[0]));
  pipePath = string(pipeDirectory) + "/pipe";
  SocketEndpoint endpoint;
  endpoint.set_name(pipePath);
  int serverClientFd = -1;
  std::thread serverListenThread(listenFn, serverSocketHandler, endpoint,
                                 &serverClientFd);

  // Wait for server to spin up
  ::usleep(1000 * 1000);
  int clientServerFd = clientSocketHandler->connect(endpoint);
  FATAL_FAIL(clientServerFd);
  serverListenThread.join();
  FATAL_FAIL(serverClientFd);

  serverCollector.reset(new BackedCollector(
      shared_ptr<BackedReader>(new BackedReader(
          serverSocketHandler,
          shared_ptr<CryptoHandler>(new CryptoHandler(
              "12345678901234567890123456789012", CLIENT_SERVER_NONCE_MSB)),
          serverClientFd)),
      shared_ptr<BackedWriter>(new BackedWriter(
          serverSocketHandler,
          shared_ptr<CryptoHandler>(new CryptoHandler(
              "12345678901234567890123456789012", SERVER_CLIENT_NONCE_MSB)),
          serverClientFd))));

  clientCollector.reset(new BackedCollector(
      shared_ptr<BackedReader>(new BackedReader(
          clientSocketHandler,
          shared_ptr<CryptoHandler>(new CryptoHandler(
              "12345678901234567890123456789012", SERVER_CLIENT_NONCE_MSB)),
          clientServerFd)),
      shared_ptr<BackedWriter>(new BackedWriter(
          clientSocketHandler,
          shared_ptr<CryptoHandler>(new CryptoHandler(
              "12345678901234567890123456789012", CLIENT_SERVER_NONCE_MSB)),
          clientServerFd))));

  SECTION("ReliableBackedTest") {
    string s(64 * 1024, '\0');
    for (int a = 0; a < 64 * 1024 - 1; a++) {
      s[a] = rand() % 26 + 'A';
    }
    s[64 * 1024 - 1] = 0;

    for (int a = 0; a < 64; a++) {
      VLOG(1) << "Writing packet " << a;
      BackedWriterWriteState r =
          serverCollector->write(string((&s[0] + a * 1024), 1024));
      if (r != BackedWriterWriteState::SUCCESS) {
        STFATAL << "Invalid write state: " << int(r);
      }
    }

    string resultConcat;
    string result;
    for (int a = 0; a < 64; a++) {
      result = clientCollector->read();
      resultConcat = resultConcat.append(result);
    }
    REQUIRE(resultConcat == s);
  }

  FATAL_FAIL(::remove(pipePath.c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}
