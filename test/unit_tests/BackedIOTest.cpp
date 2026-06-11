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
  REQUIRE(conn.write(Packet(1, "ignored")) ==
          true);  // Data is buffered even when disconnected

  conn.shutdown();
}

TEST_CASE("BackedWriter buffers when disconnected until limit", "[BackedIO]") {
  auto handler = make_shared<InMemorySocketHandler>();
  const int fd = handler->createChannel();
  const string key = "12345678901234567890123456789012";
  auto crypto = make_shared<CryptoHandler>(key, 0);

  auto writer = make_shared<BackedWriter>(handler, crypto, fd);

  string chunk(1024 * 1024, 'x');  // 1MB chunks

  // Deliver some data while connected; this backup of already-delivered data
  // must not eat into the disconnect headroom.
  for (int i = 0; i < 8; i++) {
    REQUIRE(writer->write(Packet(i, chunk)) == BackedWriterWriteState::SUCCESS);
  }

  // Disconnect
  writer->invalidateSocket();
  REQUIRE(writer->hasBufferCapacity(1024));

  // The full disconnect buffer should be available for post-disconnect data.
  int buffered = 0;
  while (true) {
    auto result = writer->write(Packet(buffered % 256, chunk));
    if (result == BackedWriterWriteState::SKIPPED) {
      break;
    }
    REQUIRE(result == BackedWriterWriteState::BUFFERED_ONLY);
    buffered++;
    // Must hit the cap by DISCONNECT_BUFFER_BYTES worth of payload.
    REQUIRE(buffered <= BackedWriter::DISCONNECT_BUFFER_BYTES / (1024 * 1024));
  }
  // Nearly all of the headroom was usable despite the pre-disconnect backup.
  REQUIRE(buffered >=
          BackedWriter::DISCONNECT_BUFFER_BYTES / (1024 * 1024) - 1);
  REQUIRE(!writer->hasBufferCapacity(2 * 1024 * 1024));

  // Reviving on a new socket resets the disconnect accounting.
  const int newFd = handler->createChannel();
  writer->revive(newFd);
  REQUIRE(writer->hasBufferCapacity(1024));
  writer->invalidateSocket();
  REQUIRE(writer->write(Packet(1, chunk)) ==
          BackedWriterWriteState::BUFFERED_ONLY);

  handler->close(fd);
}

TEST_CASE("writeAllOrThrow retries ETIMEDOUT when patient", "[SocketHandler]") {
  // On macOS, a unix socket whose peer stops draining for a long time
  // surfaces ETIMEDOUT from send() even though the connection is intact.
  class TimeoutThenOkHandler : public InMemorySocketHandler {
   public:
    int failures = 0;
    ssize_t write(int fd, const void* buf, size_t count) override {
      if (failures > 0) {
        failures--;
        SetErrno(ETIMEDOUT);
        return -1;
      }
      return InMemorySocketHandler::write(fd, buf, count);
    }
  };
  auto handler = make_shared<TimeoutThenOkHandler>();
  const int fd = handler->createChannel();
  const string data = "hello";

  // With timeout disabled (infinite patience), ETIMEDOUT is retried.
  handler->failures = 3;
  REQUIRE_NOTHROW(
      handler->writeAllOrThrow(fd, data.data(), data.length(), false));
  string echoed(data.length(), '\0');
  REQUIRE(handler->read(fd, &echoed[0], echoed.length()) ==
          (ssize_t)data.length());
  REQUIRE(echoed == data);

  // With a timeout requested, ETIMEDOUT still fails fast.
  handler->failures = 3;
  REQUIRE_THROWS(
      handler->writeAllOrThrow(fd, data.data(), data.length(), true));
}

TEST_CASE("Connection severs instead of dying on unexpected read errno",
          "[Connection]") {
  // A client waking from sleep can see errnos like ENETDOWN; the connection
  // must sever (and later reconnect) rather than tear down the session.
  class ErrnoReadHandler : public InMemorySocketHandler {
   public:
    int err = ENETDOWN;
    bool hasData(int) override { return true; }
    ssize_t read(int, void*, size_t) override {
      SetErrno(err);
      return -1;
    }
  };
  const string key = "12345678901234567890123456789012";

  for (int err : {ENETDOWN, ENETUNREACH, EINVAL}) {
    auto handler = make_shared<ErrnoReadHandler>();
    handler->err = err;
    const int fd = handler->createChannel();
    auto reader = make_shared<BackedReader>(
        handler, make_shared<CryptoHandler>(key, 0), fd);
    auto writer = make_shared<BackedWriter>(
        handler, make_shared<CryptoHandler>(key, 0), fd);
    TestConnection connection(handler, reader, writer, fd, key);

    Packet packet;
    REQUIRE_NOTHROW(connection.readPacket(&packet));
    REQUIRE(connection.isDisconnected());
    connection.shutdown();
  }
}

TEST_CASE("BackedWriter trims old data when connected and buffer exceeds 64MB",
          "[BackedIO]") {
  auto handler = make_shared<InMemorySocketHandler>();
  const int fd = handler->createChannel();
  const string key = "12345678901234567890123456789012";
  auto crypto = make_shared<CryptoHandler>(key, 0);

  auto writer = make_shared<BackedWriter>(handler, crypto, fd);

  // Write 70MB of data while connected (exceeds 64MB MAX_BACKUP_BYTES)
  string chunk(1024 * 1024, 'x');  // 1MB chunks
  for (int i = 0; i < 70; i++) {
    Packet p(i, chunk);
    auto result = writer->write(p);
    REQUIRE(result == BackedWriterWriteState::SUCCESS);
  }

  // Verify sequence number reflects all writes
  REQUIRE(writer->getSequenceNumber() == 70);

  // Disconnect and try to recover - should only have ~64MB worth of messages
  writer->invalidateSocket();

  // Request recovery of all 70 messages - should throw because old ones were
  // trimmed
  bool threw = false;
  try {
    writer->recover(0);  // Try to recover from sequence 0
  } catch (const std::runtime_error& e) {
    // Expected: "Client is too far behind server"
    threw = true;
  }
  REQUIRE(threw);

  // Recovery from a recent sequence should work
  auto recovered = writer->recover(writer->getSequenceNumber() - 10);
  REQUIRE(recovered.size() == 10);

  handler->close(fd);
}
