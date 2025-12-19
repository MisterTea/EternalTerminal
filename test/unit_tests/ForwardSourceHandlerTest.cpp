#include <cerrno>
#include <cstring>
#include <deque>
#include <set>
#include <utility>
#include <vector>

#include "ForwardSourceHandler.hpp"
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
  void enqueueAccept(int fd) { acceptQueue.push_back(fd); }
  void setEndpointFds(std::set<int> fds) { endpointFds = std::move(fds); }

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

  set<int> listen(const SocketEndpoint& endpoint) override {
    listenCalls.push_back(endpoint);
    return {100};
  }

  set<int> getEndpointFds(const SocketEndpoint& /*endpoint*/) override {
    return endpointFds;
  }

  int accept(int fd) override {
    acceptCallFds.push_back(fd);
    if (acceptQueue.empty()) {
      return -1;
    }
    int result = acceptQueue.front();
    acceptQueue.pop_front();
    return result;
  }

  void stopListening(const SocketEndpoint& endpoint) override {
    stopListeningCalls.push_back(endpoint);
  }

  void close(int fd) override { closedFds.push_back(fd); }

  vector<int> getActiveSockets() override { return {}; }

  std::deque<bool> hasDataQueue;
  std::deque<MockRead> readQueue;
  std::deque<int> acceptQueue;
  std::set<int> endpointFds;
  std::vector<MockRead> performedReads;
  std::vector<std::string> writes;
  std::vector<int> closedFds;
  std::vector<int> acceptCallFds;
  std::vector<SocketEndpoint> listenCalls;
  std::vector<SocketEndpoint> stopListeningCalls;
};
}  // namespace

TEST_CASE("ForwardSourceHandler calls listen on construction",
          "[ForwardSourceHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  SocketEndpoint source;
  source.set_name("localhost");
  source.set_port(8080);
  SocketEndpoint destination;
  destination.set_name("remote");
  destination.set_port(9090);

  ForwardSourceHandler handler(socketHandler, source, destination);

  REQUIRE(socketHandler->listenCalls.size() == 1);
  REQUIRE(socketHandler->listenCalls[0].name() == source.name());
  REQUIRE(socketHandler->listenCalls[0].port() == source.port());
  REQUIRE(handler.getDestination().name() == destination.name());
  REQUIRE(handler.getDestination().port() == destination.port());
}

TEST_CASE("ForwardSourceHandler stops listening on destruction",
          "[ForwardSourceHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  SocketEndpoint source;
  source.set_name("localhost");
  source.set_port(8080);
  SocketEndpoint destination;
  destination.set_name("remote");
  destination.set_port(9090);

  { ForwardSourceHandler handler(socketHandler, source, destination); }

  REQUIRE(socketHandler->stopListeningCalls.size() == 1);
  REQUIRE(socketHandler->stopListeningCalls[0].name() == source.name());
  REQUIRE(socketHandler->stopListeningCalls[0].port() == source.port());
}

TEST_CASE("ForwardSourceHandler listen accepts new connections",
          "[ForwardSourceHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  socketHandler->setEndpointFds({100});
  socketHandler->enqueueAccept(42);

  SocketEndpoint source;
  source.set_name("localhost");
  source.set_port(8080);
  SocketEndpoint destination;
  destination.set_name("remote");
  destination.set_port(9090);
  ForwardSourceHandler handler(socketHandler, source, destination);

  int fd = handler.listen();

  REQUIRE(fd == 42);
  REQUIRE(handler.hasUnassignedFd(42));
  REQUIRE_FALSE(handler.hasUnassignedFd(99));
}

TEST_CASE("ForwardSourceHandler listen returns -1 when no connections",
          "[ForwardSourceHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  socketHandler->setEndpointFds({100});

  SocketEndpoint source;
  source.set_name("localhost");
  source.set_port(8080);
  SocketEndpoint destination;
  destination.set_name("remote");
  destination.set_port(9090);
  ForwardSourceHandler handler(socketHandler, source, destination);

  int fd = handler.listen();

  REQUIRE(fd == -1);
}

TEST_CASE("ForwardSourceHandler closeUnassignedFd closes and removes fd",
          "[ForwardSourceHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  socketHandler->setEndpointFds({100});
  socketHandler->enqueueAccept(42);

  SocketEndpoint source;
  source.set_name("localhost");
  source.set_port(8080);
  SocketEndpoint destination;
  destination.set_name("remote");
  destination.set_port(9090);
  ForwardSourceHandler handler(socketHandler, source, destination);

  int fd = handler.listen();
  REQUIRE(handler.hasUnassignedFd(fd));

  handler.closeUnassignedFd(fd);

  REQUIRE_FALSE(handler.hasUnassignedFd(fd));
  REQUIRE(socketHandler->closedFds.size() == 1);
  REQUIRE(socketHandler->closedFds[0] == 42);
}

TEST_CASE("ForwardSourceHandler addSocket maps socket ID to fd",
          "[ForwardSourceHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  socketHandler->setEndpointFds({100});
  socketHandler->enqueueAccept(42);

  SocketEndpoint source;
  source.set_name("localhost");
  source.set_port(8080);
  SocketEndpoint destination;
  destination.set_name("remote");
  destination.set_port(9090);
  ForwardSourceHandler handler(socketHandler, source, destination);

  int fd = handler.listen();
  REQUIRE(handler.hasUnassignedFd(fd));

  handler.addSocket(123, fd);

  REQUIRE_FALSE(handler.hasUnassignedFd(fd));
}

TEST_CASE("ForwardSourceHandler update reads data from sockets",
          "[ForwardSourceHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  socketHandler->setEndpointFds({100});
  socketHandler->enqueueAccept(42);

  SocketEndpoint source;
  source.set_name("localhost");
  source.set_port(8080);
  SocketEndpoint destination;
  destination.set_name("remote");
  destination.set_port(9090);
  ForwardSourceHandler handler(socketHandler, source, destination);

  int fd = handler.listen();
  handler.addSocket(123, fd);

  socketHandler->enqueueHasData(true);
  socketHandler->enqueueRead(5, "hello");
  socketHandler->enqueueHasData(false);

  std::vector<PortForwardData> data;
  handler.update(&data);

  REQUIRE(data.size() == 1);
  REQUIRE(data[0].socketid() == 123);
  REQUIRE(data[0].sourcetodestination() == true);
  REQUIRE(data[0].buffer() == "hello");
  REQUIRE_FALSE(data[0].closed());
  REQUIRE_FALSE(data[0].has_error());
}

TEST_CASE("ForwardSourceHandler update detects closed sockets",
          "[ForwardSourceHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  socketHandler->setEndpointFds({100});
  socketHandler->enqueueAccept(42);

  SocketEndpoint source;
  source.set_name("localhost");
  source.set_port(8080);
  SocketEndpoint destination;
  destination.set_name("remote");
  destination.set_port(9090);
  ForwardSourceHandler handler(socketHandler, source, destination);

  int fd = handler.listen();
  handler.addSocket(123, fd);

  socketHandler->enqueueHasData(true);
  socketHandler->enqueueRead(0);

  std::vector<PortForwardData> data;
  handler.update(&data);

  REQUIRE(data.size() == 1);
  REQUIRE(data[0].socketid() == 123);
  REQUIRE(data[0].closed() == true);
  REQUIRE(socketHandler->closedFds.size() == 1);
  REQUIRE(socketHandler->closedFds[0] == 42);
}

TEST_CASE("ForwardSourceHandler update propagates read errors",
          "[ForwardSourceHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  socketHandler->setEndpointFds({100});
  socketHandler->enqueueAccept(42);

  SocketEndpoint source;
  source.set_name("localhost");
  source.set_port(8080);
  SocketEndpoint destination;
  destination.set_name("remote");
  destination.set_port(9090);
  ForwardSourceHandler handler(socketHandler, source, destination);

  int fd = handler.listen();
  handler.addSocket(123, fd);

  socketHandler->enqueueHasData(true);
  socketHandler->enqueueRead(-1, "", EIO);

  std::vector<PortForwardData> data;
  handler.update(&data);

  REQUIRE(data.size() == 1);
  REQUIRE(data[0].socketid() == 123);
  REQUIRE(data[0].error() == std::string(strerror(EIO)));
  REQUIRE(socketHandler->closedFds.size() == 1);
  REQUIRE(socketHandler->closedFds[0] == 42);
}

TEST_CASE("ForwardSourceHandler update ignores transient EAGAIN reads",
          "[ForwardSourceHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  socketHandler->setEndpointFds({100});
  socketHandler->enqueueAccept(42);

  SocketEndpoint source;
  source.set_name("localhost");
  source.set_port(8080);
  SocketEndpoint destination;
  destination.set_name("remote");
  destination.set_port(9090);
  ForwardSourceHandler handler(socketHandler, source, destination);

  int fd = handler.listen();
  handler.addSocket(123, fd);

  socketHandler->enqueueHasData(true);
  socketHandler->enqueueRead(-1, "", EAGAIN);
  socketHandler->enqueueHasData(false);

  std::vector<PortForwardData> data;
  handler.update(&data);

  REQUIRE(data.empty());
  REQUIRE(socketHandler->closedFds.empty());
}

TEST_CASE("ForwardSourceHandler sendDataOnSocket writes to socket",
          "[ForwardSourceHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  socketHandler->setEndpointFds({100});
  socketHandler->enqueueAccept(42);

  SocketEndpoint source;
  source.set_name("localhost");
  source.set_port(8080);
  SocketEndpoint destination;
  destination.set_name("remote");
  destination.set_port(9090);
  ForwardSourceHandler handler(socketHandler, source, destination);

  int fd = handler.listen();
  handler.addSocket(123, fd);

  handler.sendDataOnSocket(123, "test data");

  REQUIRE(socketHandler->writes.size() == 1);
  REQUIRE(socketHandler->writes[0] == "test data");
}

TEST_CASE("ForwardSourceHandler closeSocket closes and removes socket",
          "[ForwardSourceHandler]") {
  auto socketHandler = std::make_shared<MockSocketHandler>();
  socketHandler->setEndpointFds({100});
  socketHandler->enqueueAccept(42);

  SocketEndpoint source;
  source.set_name("localhost");
  source.set_port(8080);
  SocketEndpoint destination;
  destination.set_name("remote");
  destination.set_port(9090);
  ForwardSourceHandler handler(socketHandler, source, destination);

  int fd = handler.listen();
  handler.addSocket(123, fd);

  handler.closeSocket(123);

  REQUIRE(socketHandler->closedFds.size() == 1);
  REQUIRE(socketHandler->closedFds[0] == 42);
}
