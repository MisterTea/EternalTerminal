#include <atomic>
#include <thread>

#include "BackedReader.hpp"
#include "BackedWriter.hpp"
#include "Connection.hpp"
#include "RawSocketUtils.hpp"
#include "SocketHandler.hpp"
#include "TestHeaders.hpp"

using namespace et;
using Catch::Matchers::Equals;

namespace {
// Simple in-memory socket handler for exercising BackedReader/BackedWriter.
class InMemorySocketHandler : public SocketHandler {
 public:
  int createChannel() {
    int fd = nextFd++;
    buffers[fd] = {};
    return fd;
  }

  void enqueue(int fd, const string& data) {
    auto& q = buffers[fd];
    for (char c : data) {
      q.push_back(c);
    }
  }

  bool hasData(int fd) override { return !buffers[fd].empty(); }

  ssize_t read(int fd, void* buf, size_t count) override {
    auto& q = buffers[fd];
    if (q.empty()) {
      SetErrno(EPIPE);
      return 0;
    }
    size_t n = std::min(count, q.size());
    for (size_t i = 0; i < n; ++i) {
      static_cast<char*>(buf)[i] = q.front();
      q.pop_front();
    }
    return n;
  }

  ssize_t write(int fd, const void* buf, size_t count) override {
    auto& q = buffers[fd];
    auto* c = static_cast<const char*>(buf);
    for (size_t i = 0; i < count; ++i) {
      q.push_back(c[i]);
    }
    return count;
  }

  int connect(const SocketEndpoint&) override { return -1; }
  set<int> listen(const SocketEndpoint&) override { return {}; }
  set<int> getEndpointFds(const SocketEndpoint&) override { return {}; }
  int accept(int) override { return -1; }
  void stopListening(const SocketEndpoint&) override {}
  void close(int) override {}
  vector<int> getActiveSockets() override { return {}; }

 private:
  std::atomic<int> nextFd{1};
  std::map<int, std::deque<char>> buffers;
};

// Minimal handler that forwards to OS file descriptors (pipes) for
// exercising SocketHandler helpers without a full socket stack.
class FdSocketHandler : public SocketHandler {
 public:
  bool hasData(int fd) override { return waitOnSocketData(fd); }

  ssize_t read(int fd, void* buf, size_t count) override {
    return ::read(fd, buf, count);
  }

  ssize_t write(int fd, const void* buf, size_t count) override {
    return ::write(fd, buf, count);
  }

  int connect(const SocketEndpoint&) override { return -1; }
  set<int> listen(const SocketEndpoint&) override { return {}; }
  set<int> getEndpointFds(const SocketEndpoint&) override { return {}; }
  int accept(int) override { return -1; }
  void stopListening(const SocketEndpoint&) override {}
  void close(int fd) override { ::close(fd); }
  vector<int> getActiveSockets() override { return {}; }
};

class TestConnection : public Connection {
 public:
  TestConnection(shared_ptr<SocketHandler> sh, shared_ptr<BackedReader> r,
                 shared_ptr<BackedWriter> w, int fd, const string& key)
      : Connection(sh, "test-id", key) {
    reader = std::move(r);
    writer = std::move(w);
    socketFd = fd;
  }

  void closeSocketAndMaybeReconnect() override { closeSocket(); }
};
}  // namespace

TEST_CASE("BackedReader and BackedWriter round trip", "[BackedIO]") {
  auto handler = make_shared<InMemorySocketHandler>();
  auto encryptCrypto = make_shared<CryptoHandler>(
      "12345678901234567890123456789012", 0 /*verbosity*/);
  auto decryptCrypto = make_shared<CryptoHandler>(
      "12345678901234567890123456789012", 0 /*verbosity*/);
  const int fd = handler->createChannel();

  BackedWriter writer(handler, encryptCrypto, fd);
  BackedReader reader(handler, decryptCrypto, fd);

  Packet input(42, "hello backed io");
  auto result = writer.write(input);
  REQUIRE(result == BackedWriterWriteState::SUCCESS);

  Packet output;
  REQUIRE(reader.read(&output) == 1);
  REQUIRE(output.getHeader() == 42);
  REQUIRE(output.getPayload() == "hello backed io");
  REQUIRE(reader.getSequenceNumber() == 1);
}

TEST_CASE("BackedWriter recovers buffered messages in order", "[BackedIO]") {
  auto handler = make_shared<InMemorySocketHandler>();
  auto encryptCrypto = make_shared<CryptoHandler>(
      "12345678901234567890123456789012", 0 /*verbosity*/);
  auto decryptCrypto = make_shared<CryptoHandler>(
      "12345678901234567890123456789012", 0 /*verbosity*/);
  const int fd = handler->createChannel();

  BackedWriter writer(handler, encryptCrypto, fd);
  Packet first(1, "first");
  Packet second(2, "second");

  REQUIRE(writer.write(first) == BackedWriterWriteState::SUCCESS);
  REQUIRE(writer.write(second) == BackedWriterWriteState::SUCCESS);

  // Simulate a dead socket so recover is allowed.
  writer.revive(-1);
  auto recovered = writer.recover(0);
  REQUIRE(recovered.size() == 2);

  Packet recoveredFirst(recovered[0]);
  recoveredFirst.decrypt(decryptCrypto);
  REQUIRE(recoveredFirst.getHeader() == 1);
  REQUIRE(recoveredFirst.getPayload() == "first");

  Packet recoveredSecond(recovered[1]);
  recoveredSecond.decrypt(decryptCrypto);
  REQUIRE(recoveredSecond.getHeader() == 2);
  REQUIRE(recoveredSecond.getPayload() == "second");
}

TEST_CASE("BackedReader revive seeds local buffer", "[BackedIO]") {
  auto handler = make_shared<InMemorySocketHandler>();
  auto encryptCrypto = make_shared<CryptoHandler>(
      "12345678901234567890123456789012", 0 /*verbosity*/);
  auto decryptCrypto = make_shared<CryptoHandler>(
      "12345678901234567890123456789012", 0 /*verbosity*/);
  const int fd = handler->createChannel();

  BackedReader reader(handler, decryptCrypto, fd);

  Packet cached(7, "cached-payload");
  cached.encrypt(encryptCrypto);
  reader.revive(fd, {cached.serialize()});

  Packet fromCache;
  REQUIRE(reader.read(&fromCache) == 1);
  REQUIRE(fromCache.getHeader() == 7);
  REQUIRE(fromCache.getPayload() == "cached-payload");
  REQUIRE(reader.getSequenceNumber() == 1);
}

TEST_CASE("RawSocketUtils readAll waits for data then returns fully",
          "[RawSocketUtils]") {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);

  const string payload = "socketutils";
  std::thread writer([&]() {
    RawSocketUtils::writeAll(fds[1], payload.data(), payload.size());
  });

  string buffer(payload.size(), '\0');
  RawSocketUtils::readAll(fds[0], &buffer[0], buffer.size());
  REQUIRE(buffer == payload);

  writer.join();
  ::close(fds[0]);
  ::close(fds[1]);
}

TEST_CASE("SocketHandler helpers read/write encoded payloads",
          "[SocketHandler]") {
  FdSocketHandler handler;
  int fds[2];
  REQUIRE(::pipe(fds) == 0);

  // Verify writeAllOrReturn followed by readAll moves the entire message.
  const string raw = "handler-data-block";
  std::thread reader([&]() {
    string buf(raw.size(), '\0');
    handler.readAll(fds[0], &buf[0], buf.size(), true);
    REQUIRE(buf == raw);
  });
  REQUIRE(handler.writeAllOrReturn(fds[1], raw.data(), raw.size()) ==
          static_cast<int>(raw.size()));
  reader.join();

  // Verify base64 helpers round trip on the same pipe.
  const string b64Input = "b64-payload";
  std::thread b64Reader([&]() {
    string buf(b64Input.size(), '\0');
    handler.readB64(fds[0], &buf[0], buf.size());
    REQUIRE(buf == b64Input);
  });
  handler.writeB64(fds[1], b64Input.data(), b64Input.size());
  b64Reader.join();

  handler.close(fds[0]);
  handler.close(fds[1]);
}

TEST_CASE("Connection writes and reads packets with backing buffers",
          "[Connection]") {
  auto handler = make_shared<InMemorySocketHandler>();
  const int fd = handler->createChannel();
  const string key = "12345678901234567890123456789012";
  auto encryptCrypto = make_shared<CryptoHandler>(key, 0);
  auto decryptCrypto = make_shared<CryptoHandler>(key, 0);

  auto reader = make_shared<BackedReader>(handler, decryptCrypto, fd);
  auto writer = make_shared<BackedWriter>(handler, encryptCrypto, fd);
  TestConnection conn(handler, reader, writer, fd, key);

  Packet pkt(55, "connection-roundtrip");
  conn.write(pkt);

  REQUIRE(conn.hasData());
  Packet out;
  REQUIRE(conn.read(&out));
  REQUIRE(out.getHeader() == 55);
  REQUIRE(out.getPayload() == "connection-roundtrip");

  conn.shutdown();
}

TEST_CASE("Connection closeSocket updates disconnected state", "[Connection]") {
  auto handler = make_shared<InMemorySocketHandler>();
  const int fd = handler->createChannel();
  const string key = "12345678901234567890123456789012";
  auto crypto = make_shared<CryptoHandler>(key, 0);

  auto reader = make_shared<BackedReader>(handler, crypto, fd);
  auto writer = make_shared<BackedWriter>(handler, crypto, fd);
  TestConnection conn(handler, reader, writer, fd, key);

  REQUIRE_FALSE(conn.isDisconnected());
  conn.closeSocket();
  REQUIRE(conn.isDisconnected());
  REQUIRE(conn.write(Packet(1, "ignored")) == false);

  conn.shutdown();
}
