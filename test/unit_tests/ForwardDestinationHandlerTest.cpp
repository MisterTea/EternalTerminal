#include <catch2/generators/catch_generators.hpp>
#include <cerrno>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>

#include "ForwardDestinationHandler.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {
struct MockRead {
  ssize_t result;
  std::string payload;
  int errnoValue;
};

class MockSocketHandler : public SocketHandler {
 public:
  void enqueueHasData(bool value) { hasDataQueue.push_back(value); }
  void enqueueRead(ssize_t result, std::string payload = "",
                   int errnoValue = 0) {
    readQueue.push_back({result, std::move(payload), errnoValue});
  }

  bool hasData(int /*fd*/) override {
    if (hasDataQueue.empty()) {
      return false;
    }
    bool value = hasDataQueue.front();
    hasDataQueue.pop_front();
    return value;
  }

  ssize_t read(int /*fd*/, void* buf, size_t count) override {
    REQUIRE_FALSE(readQueue.empty());
    auto read = readQueue.front();
    readQueue.pop_front();
    SetErrno(read.errnoValue);
    if (read.result > 0) {
      REQUIRE(read.payload.size() == static_cast<size_t>(read.result));
      REQUIRE(read.result <= static_cast<ssize_t>(count));
      memcpy(buf, read.payload.data(), read.payload.size());
    }
    performedReads.push_back(read);
    return read.result;
  }

  ssize_t write(int /*fd*/, const void* buf, size_t count) override {
    writes.emplace_back(static_cast<const char*>(buf), count);
    SetErrno(0);
    return count;
  }

  int connect(const SocketEndpoint& /*endpoint*/) override { return -1; }
  set<int> listen(const SocketEndpoint& /*endpoint*/) override { return {}; }
  set<int> getEndpointFds(const SocketEndpoint& /*endpoint*/) override {
    return {};
  }
  int accept(int /*fd*/) override { return -1; }
  void stopListening(const SocketEndpoint& /*endpoint*/) override {}
  void close(int fd) override { closedFds.push_back(fd); }
  vector<int> getActiveSockets() override { return {}; }

  std::deque<bool> hasDataQueue;
  std::deque<MockRead> readQueue;
  std::vector<MockRead> performedReads;
  std::vector<std::string> writes;
  std::vector<int> closedFds;
};
}  // namespace

TEST_CASE("ForwardDestinationHandler closes its socket on destruction",
          "[ForwardDestinationHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  { ForwardDestinationHandler handler(socketHandler, 42, 123); }
  CHECK(socketHandler->closedFds == std::vector<int>{42});
}

TEST_CASE("ForwardDestinationHandler does not close released sockets twice",
          "[ForwardDestinationHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  {
    ForwardDestinationHandler handler(socketHandler, 42, 123);
    SECTION("Explicit close") {
      handler.close();
      handler.close();
    }
    SECTION("Peer EOF") {
      socketHandler->enqueueHasData(true);
      socketHandler->enqueueRead(0);
      std::vector<PortForwardData> data;
      handler.update(&data);
    }
    CHECK(handler.getFd() == -1);
  }
  CHECK(socketHandler->closedFds == std::vector<int>{42});
}

TEST_CASE("ForwardDestinationHandler forwards outbound payloads",
          "[ForwardDestinationHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  ForwardDestinationHandler handler(socketHandler, /*fd=*/123, /*socketId=*/42);

  handler.write("payload");

  REQUIRE(socketHandler->writes.size() == 1);
  REQUIRE(socketHandler->writes[0] == "payload");
  REQUIRE(socketHandler->closedFds.empty());
}

TEST_CASE("ForwardDestinationHandler captures inbound data frames",
          "[ForwardDestinationHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  ForwardDestinationHandler handler(socketHandler, /*fd=*/17, /*socketId=*/99);

  socketHandler->enqueueHasData(true);
  socketHandler->enqueueRead(5, "hello");
  socketHandler->enqueueHasData(false);

  std::vector<PortForwardData> responses;
  handler.update(&responses);

  REQUIRE(responses.size() == 1);
  const auto& frame = responses[0];
  REQUIRE(frame.socketid() == 99);
  REQUIRE_FALSE(frame.sourcetodestination());
  REQUIRE(frame.buffer() == "hello");
  REQUIRE_FALSE(frame.closed());
  REQUIRE_FALSE(frame.has_error());
  REQUIRE(handler.getFd() == 17);
  REQUIRE(socketHandler->closedFds.empty());
}

TEST_CASE("ForwardDestinationHandler reports closes and clears fd",
          "[ForwardDestinationHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  ForwardDestinationHandler handler(socketHandler, /*fd=*/50, /*socketId=*/7);

  socketHandler->enqueueHasData(true);
  socketHandler->enqueueRead(0);

  std::vector<PortForwardData> responses;
  handler.update(&responses);

  REQUIRE(responses.size() == 1);
  REQUIRE(responses[0].socketid() == 7);
  REQUIRE(responses[0].closed());
  REQUIRE(handler.getFd() == -1);
  REQUIRE(socketHandler->closedFds == std::vector<int>{50});
}

TEST_CASE("ForwardDestinationHandler propagates read errors",
          "[ForwardDestinationHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  ForwardDestinationHandler handler(socketHandler, /*fd=*/60, /*socketId=*/11);

  socketHandler->enqueueHasData(true);
  socketHandler->enqueueRead(-1, "", EIO);

  std::vector<PortForwardData> responses;
  handler.update(&responses);

  REQUIRE(responses.size() == 1);
  REQUIRE(responses[0].socketid() == 11);
  REQUIRE(responses[0].error() == std::string(strerror(EIO)));
  REQUIRE(responses[0].buffer().empty());
  REQUIRE_FALSE(responses[0].closed());
  REQUIRE(handler.getFd() == -1);
  REQUIRE(socketHandler->closedFds == std::vector<int>{60});
}

TEST_CASE("ForwardDestinationHandler ignores transient EAGAIN reads",
          "[ForwardDestinationHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  ForwardDestinationHandler handler(socketHandler, /*fd=*/70, /*socketId=*/5);

  socketHandler->enqueueHasData(true);
  socketHandler->enqueueRead(-1, "", EAGAIN);
  socketHandler->enqueueHasData(false);

  std::vector<PortForwardData> responses;
  handler.update(&responses);

  REQUIRE(responses.empty());
  REQUIRE(handler.getFd() == 70);
  REQUIRE(socketHandler->closedFds.empty());
}

TEST_CASE("ForwardDestinationHandler yields after a bounded data batch",
          "[ForwardDestinationHandler][issue298]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  ForwardDestinationHandler handler(socketHandler, /*fd=*/71, /*socketId=*/6);

  const string chunk(16 * 1024, 'x');
  for (int i = 0; i < 5; ++i) {
    socketHandler->enqueueHasData(true);
    socketHandler->enqueueRead(chunk.size(), chunk);
  }

  std::vector<PortForwardData> responses;
  handler.update(&responses);

  REQUIRE(responses.size() == 4);
  REQUIRE(socketHandler->performedReads.size() == 4);
}

TEST_CASE("X11 forwarding replaces only an authenticated setup cookie",
          "[ForwardDestinationHandler][X11]") {
  const bool little = GENERATE(true, false);
  X11Forward x11;
  x11.proxyCookie = string(X11CookieSize, 'p');
  x11.localCookie = string(X11CookieSize, 'l');
  string setup(12, '\0');
  setup[0] = little ? 'l' : 'B';
  setup[little ? 2 : 3] = 11;
  setup[little ? 6 : 7] = sizeof(X11AuthProtocol) - 1;
  setup[little ? 8 : 9] = X11CookieSize;
  setup += X11AuthProtocol;
  setup += string(2, '\0');
  const size_t cookieOffset = setup.size();
  setup += x11.proxyCookie;
  const auto socket = make_shared<MockSocketHandler>();

  SECTION("split setup and additional data are passed through in order") {
    ForwardDestinationHandler handler(socket, 42, 123, nullptr, &x11);
    handler.write(setup.substr(0, 8));
    handler.write(setup.substr(8, 32));
    REQUIRE(socket->writes.empty());
    handler.write(setup.substr(40) + "post-setup");
    handler.write("later");
    setup.replace(cookieOffset, X11CookieSize, x11.localCookie);
    REQUIRE(socket->writes == vector<string>{setup + "post-setup", "later"});
  }

  SECTION("a bad cookie closes the connection without writing to X11") {
    ForwardDestinationHandler handler(socket, 42, 123, nullptr, &x11);
    setup[cookieOffset] = 'x';
    handler.write(setup);
    REQUIRE(socket->writes.empty());
    REQUIRE(handler.rejectedX11Setup());
    REQUIRE(socket->closedFds == vector<int>{42});
  }

  SECTION("an expired setup is rejected") {
    x11.expires = std::chrono::steady_clock::now();
    ForwardDestinationHandler handler(socket, 42, 123, nullptr, &x11);
    handler.write(setup);
    REQUIRE(socket->writes.empty());
    REQUIRE(handler.rejectedX11Setup());
  }
}
