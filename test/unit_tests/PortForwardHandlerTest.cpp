#include "PortForwardHandler.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {
class FakePortForwardSocketHandler : public SocketHandler {
 public:
  struct ReadAction {
    int result;
    string data;
    int err;
  };

  string endpointKey(const SocketEndpoint& endpoint) const {
    string key = endpoint.has_name() ? endpoint.name() : "";
    key += "|";
    key += endpoint.has_port() ? std::to_string(endpoint.port()) : "";
    return key;
  }

  void queueRead(int fd, int result, const string& data = "", int err = 0) {
    readQueue[fd].push_back({result, data, err});
  }

  void queueAccept(int listenFd, int resultFd) {
    acceptQueue[listenFd].push_back(resultFd);
  }

  void setConnectResult(int fd) { nextConnectFd = fd; }

  bool hasData(int fd) override {
    auto it = readQueue.find(fd);
    return it != readQueue.end() && !it->second.empty();
  }

  ssize_t read(int fd, void* buf, size_t count) override {
    auto it = readQueue.find(fd);
    if (it == readQueue.end() || it->second.empty()) {
      SetErrno(EAGAIN);
      return -1;
    }
    auto action = it->second.front();
    it->second.pop_front();
    SetErrno(action.err);
    if (action.result > 0) {
      auto copyLen = std::min<size_t>(action.result, count);
      memcpy(buf, action.data.data(), copyLen);
    }
    return action.result;
  }

  ssize_t write(int fd, const void* buf, size_t count) override {
    writes[fd].emplace_back((const char*)buf, count);
    return count;
  }

  int connect(const SocketEndpoint& endpoint) override {
    connectEndpoints.push_back(endpoint);
    int fd = nextConnectFd;
    nextConnectFd = -1;
    return fd;
  }

  set<int> listen(const SocketEndpoint& endpoint) override {
    int fd = nextListenFd++;
    auto key = endpointKey(endpoint);
    listenerFds[key] = {fd};
    return listenerFds[key];
  }

  set<int> getEndpointFds(const SocketEndpoint& endpoint) override {
    auto key = endpointKey(endpoint);
    auto it = listenerFds.find(key);
    if (it == listenerFds.end()) {
      return {};
    }
    return it->second;
  }

  int accept(int fd) override {
    auto& queue = acceptQueue[fd];
    if (queue.empty()) {
      SetErrno(EAGAIN);
      return -1;
    }
    int result = queue.front();
    queue.pop_front();
    if (result >= 0) {
      activeSockets.insert(result);
    }
    return result;
  }

  void stopListening(const SocketEndpoint& endpoint) override {
    stoppedEndpoints.push_back(endpoint);
  }

  void close(int fd) override {
    closedFds.push_back(fd);
    activeSockets.erase(fd);
  }

  vector<int> getActiveSockets() override {
    return vector<int>(activeSockets.begin(), activeSockets.end());
  }

  std::unordered_map<int, std::deque<ReadAction>> readQueue;
  std::unordered_map<int, std::deque<int>> acceptQueue;
  std::unordered_map<int, vector<string>> writes;
  std::unordered_map<string, set<int>> listenerFds;
  std::set<int> activeSockets;
  vector<int> closedFds;
  vector<SocketEndpoint> stoppedEndpoints;
  vector<SocketEndpoint> connectEndpoints;
  int nextListenFd = 100;
  int nextConnectFd = -1;
};

class FakeConnection : public Connection {
 public:
  FakeConnection() : Connection(nullptr, "", "") {}

  void writePacket(const Packet& packet) override {
    sentPackets.push_back(packet);
  }

  vector<Packet> sentPackets;
};

}  // namespace

TEST_CASE("PortForwardHandler constructor", "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();

  PortForwardHandler handler(networkHandler, pipeHandler);

  // Handler should be constructed without errors
  REQUIRE(true);
}

TEST_CASE("PortForwardHandler update with no handlers",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  vector<PortForwardDestinationRequest> requests;
  vector<PortForwardData> dataToSend;

  handler.update(&requests, &dataToSend);

  CHECK(requests.empty());
  CHECK(dataToSend.empty());
}

TEST_CASE("PortForwardHandler createSource with port forward",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  PortForwardSourceRequest request;
  SocketEndpoint source;
  source.set_port(8080);
  *request.mutable_source() = source;

  SocketEndpoint destination;
  destination.set_port(9090);
  *request.mutable_destination() = destination;

  PortForwardSourceResponse response =
      handler.createSource(request, nullptr, 1000, 1000);

  CHECK_FALSE(response.has_error());
}

// SKIPPED: Test creates actual Unix sockets and chmod fails in some
// environments (WSL) Error: chmod fails with EINVAL (22) on Unix domain sockets
// in WSL2
//
// TEST_CASE("PortForwardHandler createSource with named pipe",
//           "[PortForwardHandler]") {
//   auto networkHandler = make_shared<FakePortForwardSocketHandler>();
//   auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
//   PortForwardHandler handler(networkHandler, pipeHandler);
//
//   PortForwardSourceRequest request;
//   SocketEndpoint destination;
//   destination.set_name("/tmp/test.sock");
//   *request.mutable_destination() = destination;
//
//   string sourceName;
//   PortForwardSourceResponse response =
//       handler.createSource(request, &sourceName, 1000, 1000);
//
//   CHECK_FALSE(response.has_error());
//   CHECK_FALSE(sourceName.empty());
// }

TEST_CASE("PortForwardHandler createSource error when source and sourceName",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  PortForwardSourceRequest request;
  SocketEndpoint source;
  source.set_name("/tmp/source.sock");
  *request.mutable_source() = source;

  SocketEndpoint destination;
  destination.set_name("/tmp/dest.sock");
  *request.mutable_destination() = destination;

  string sourceName;
  PortForwardSourceResponse response =
      handler.createSource(request, &sourceName, 1000, 1000);

  CHECK(response.has_error());
}

TEST_CASE("PortForwardHandler createDestination with port (IPv6)",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  networkHandler->setConnectResult(42);

  PortForwardDestinationRequest request;
  SocketEndpoint destination;
  destination.set_port(8080);
  *request.mutable_destination() = destination;
  request.set_fd(100);

  PortForwardDestinationResponse response = handler.createDestination(request);

  CHECK(response.clientfd() == 100);
  CHECK_FALSE(response.has_error());
  CHECK(response.has_socketid());
  REQUIRE(networkHandler->connectEndpoints.size() == 1);
  CHECK(networkHandler->connectEndpoints[0].name() == "::1");
  CHECK(networkHandler->connectEndpoints[0].port() == 8080);
}

TEST_CASE("PortForwardHandler createDestination with port fallback to IPv4",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  // First connect (IPv6) fails, second (IPv4) succeeds
  networkHandler->setConnectResult(42);

  PortForwardDestinationRequest request;
  SocketEndpoint destination;
  destination.set_port(8080);
  *request.mutable_destination() = destination;
  request.set_fd(100);

  // Simulate IPv6 failure by having first connect return -1, then second
  // succeeds
  networkHandler->nextConnectFd = -1;
  PortForwardDestinationResponse response1 = handler.createDestination(request);

  // This will fail because both attempts fail
  CHECK(response1.has_error());
  REQUIRE(networkHandler->connectEndpoints.size() == 2);
  CHECK(networkHandler->connectEndpoints[0].name() == "::1");
  CHECK(networkHandler->connectEndpoints[1].name() == "127.0.0.1");
}

TEST_CASE("PortForwardHandler createDestination with pipe",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  pipeHandler->setConnectResult(55);

  PortForwardDestinationRequest request;
  SocketEndpoint destination;
  destination.set_name("/tmp/test.sock");
  *request.mutable_destination() = destination;
  request.set_fd(200);

  PortForwardDestinationResponse response = handler.createDestination(request);

  CHECK(response.clientfd() == 200);
  CHECK_FALSE(response.has_error());
  CHECK(response.has_socketid());
  REQUIRE(pipeHandler->connectEndpoints.size() == 1);
  CHECK(pipeHandler->connectEndpoints[0].name() == "/tmp/test.sock");
}

TEST_CASE("PortForwardHandler createDestination connection failed",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  networkHandler->setConnectResult(-1);

  PortForwardDestinationRequest request;
  SocketEndpoint destination;
  destination.set_port(8080);
  *request.mutable_destination() = destination;
  request.set_fd(100);

  PortForwardDestinationResponse response = handler.createDestination(request);

  CHECK(response.clientfd() == 100);
  CHECK(response.has_error());
  CHECK_FALSE(response.has_socketid());
}

TEST_CASE("PortForwardHandler handlePacket PORT_FORWARD_DATA destination",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);
  auto connection = make_shared<FakeConnection>();

  // First create a destination
  networkHandler->setConnectResult(42);
  PortForwardDestinationRequest destRequest;
  SocketEndpoint destination;
  destination.set_port(8080);
  *destRequest.mutable_destination() = destination;
  destRequest.set_fd(100);
  PortForwardDestinationResponse destResponse =
      handler.createDestination(destRequest);
  REQUIRE_FALSE(destResponse.has_error());
  int socketId = destResponse.socketid();

  // Send data to the destination
  PortForwardData data;
  data.set_sourcetodestination(true);
  data.set_socketid(socketId);
  data.set_buffer("test data");

  Packet packet(uint8_t(TerminalPacketType::PORT_FORWARD_DATA),
                protoToString(data));
  handler.handlePacket(packet, connection);

  // Check that data was written to the socket
  REQUIRE(networkHandler->writes.count(42) == 1);
  CHECK(networkHandler->writes[42][0] == "test data");
}

TEST_CASE("PortForwardHandler handlePacket PORT_FORWARD_DATA close destination",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);
  auto connection = make_shared<FakeConnection>();

  // First create a destination
  networkHandler->setConnectResult(42);
  PortForwardDestinationRequest destRequest;
  SocketEndpoint destination;
  destination.set_port(8080);
  *destRequest.mutable_destination() = destination;
  destRequest.set_fd(100);
  PortForwardDestinationResponse destResponse =
      handler.createDestination(destRequest);
  REQUIRE_FALSE(destResponse.has_error());
  int socketId = destResponse.socketid();

  // Send close signal
  PortForwardData data;
  data.set_sourcetodestination(true);
  data.set_socketid(socketId);
  data.set_closed(true);

  Packet packet(uint8_t(TerminalPacketType::PORT_FORWARD_DATA),
                protoToString(data));
  handler.handlePacket(packet, connection);

  // Check that socket was closed
  CHECK(std::find(networkHandler->closedFds.begin(),
                  networkHandler->closedFds.end(),
                  42) != networkHandler->closedFds.end());
}

TEST_CASE("PortForwardHandler handlePacket PORT_FORWARD_DATA error destination",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);
  auto connection = make_shared<FakeConnection>();

  // First create a destination
  networkHandler->setConnectResult(42);
  PortForwardDestinationRequest destRequest;
  SocketEndpoint destination;
  destination.set_port(8080);
  *destRequest.mutable_destination() = destination;
  destRequest.set_fd(100);
  PortForwardDestinationResponse destResponse =
      handler.createDestination(destRequest);
  REQUIRE_FALSE(destResponse.has_error());
  int socketId = destResponse.socketid();

  // Send error signal
  PortForwardData data;
  data.set_sourcetodestination(true);
  data.set_socketid(socketId);
  data.set_error("connection error");

  Packet packet(uint8_t(TerminalPacketType::PORT_FORWARD_DATA),
                protoToString(data));
  handler.handlePacket(packet, connection);

  // Check that socket was closed
  CHECK(std::find(networkHandler->closedFds.begin(),
                  networkHandler->closedFds.end(),
                  42) != networkHandler->closedFds.end());
}

TEST_CASE("PortForwardHandler handlePacket PORT_FORWARD_DESTINATION_REQUEST",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);
  auto connection = make_shared<FakeConnection>();

  networkHandler->setConnectResult(42);

  PortForwardDestinationRequest request;
  SocketEndpoint destination;
  destination.set_port(8080);
  *request.mutable_destination() = destination;
  request.set_fd(100);

  Packet packet(uint8_t(TerminalPacketType::PORT_FORWARD_DESTINATION_REQUEST),
                protoToString(request));
  handler.handlePacket(packet, connection);

  // Check that a response was sent
  REQUIRE(connection->sentPackets.size() == 1);
  CHECK(connection->sentPackets[0].getHeader() ==
        uint8_t(TerminalPacketType::PORT_FORWARD_DESTINATION_RESPONSE));
}

TEST_CASE("PortForwardHandler handlePacket PORT_FORWARD_DESTINATION_RESPONSE",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);
  auto connection = make_shared<FakeConnection>();

  // Create a source first
  PortForwardSourceRequest sourceRequest;
  SocketEndpoint source;
  source.set_port(8080);
  *sourceRequest.mutable_source() = source;
  SocketEndpoint destination;
  destination.set_port(9090);
  *sourceRequest.mutable_destination() = destination;
  handler.createSource(sourceRequest, nullptr, 1000, 1000);

  // Simulate accepting a connection on the source
  vector<PortForwardDestinationRequest> requests;
  vector<PortForwardData> dataToSend;

  // Get the listen fd
  auto fds = networkHandler->getEndpointFds(source);
  REQUIRE_FALSE(fds.empty());
  int listenFd = *(fds.begin());

  // Queue an accept
  networkHandler->queueAccept(listenFd, 123);
  handler.update(&requests, &dataToSend);

  REQUIRE(requests.size() == 1);
  int clientFd = requests[0].fd();

  // Now handle the response
  PortForwardDestinationResponse response;
  response.set_clientfd(clientFd);
  response.set_socketid(456);

  Packet packet(uint8_t(TerminalPacketType::PORT_FORWARD_DESTINATION_RESPONSE),
                protoToString(response));
  handler.handlePacket(packet, connection);

  // The socket should be mapped now, verify by sending data
  handler.sendDataToSourceOnSocket(456, "test");
  REQUIRE(networkHandler->writes.count(clientFd) == 1);
  CHECK(networkHandler->writes[clientFd][0] == "test");
}

TEST_CASE(
    "PortForwardHandler handlePacket PORT_FORWARD_DESTINATION_RESPONSE with "
    "error",
    "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);
  auto connection = make_shared<FakeConnection>();

  // Create a source first
  PortForwardSourceRequest sourceRequest;
  SocketEndpoint source;
  source.set_port(8080);
  *sourceRequest.mutable_source() = source;
  SocketEndpoint destination;
  destination.set_port(9090);
  *sourceRequest.mutable_destination() = destination;
  handler.createSource(sourceRequest, nullptr, 1000, 1000);

  // Simulate accepting a connection on the source
  vector<PortForwardDestinationRequest> requests;
  vector<PortForwardData> dataToSend;

  auto fds = networkHandler->getEndpointFds(source);
  REQUIRE_FALSE(fds.empty());
  int listenFd = *(fds.begin());

  networkHandler->queueAccept(listenFd, 123);
  handler.update(&requests, &dataToSend);

  REQUIRE(requests.size() == 1);
  int clientFd = requests[0].fd();

  // Response with error
  PortForwardDestinationResponse response;
  response.set_clientfd(clientFd);
  response.set_error("connection failed");

  Packet packet(uint8_t(TerminalPacketType::PORT_FORWARD_DESTINATION_RESPONSE),
                protoToString(response));
  handler.handlePacket(packet, connection);

  // The client fd should be closed
  CHECK(std::find(networkHandler->closedFds.begin(),
                  networkHandler->closedFds.end(),
                  clientFd) != networkHandler->closedFds.end());
}

TEST_CASE("PortForwardHandler closeSourceFd", "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  // Create a source
  PortForwardSourceRequest sourceRequest;
  SocketEndpoint source;
  source.set_port(8080);
  *sourceRequest.mutable_source() = source;
  SocketEndpoint destination;
  destination.set_port(9090);
  *sourceRequest.mutable_destination() = destination;
  handler.createSource(sourceRequest, nullptr, 1000, 1000);

  // Accept a connection
  vector<PortForwardDestinationRequest> requests;
  vector<PortForwardData> dataToSend;

  auto fds = networkHandler->getEndpointFds(source);
  REQUIRE_FALSE(fds.empty());
  int listenFd = *(fds.begin());

  networkHandler->queueAccept(listenFd, 123);
  handler.update(&requests, &dataToSend);

  REQUIRE(requests.size() == 1);
  int clientFd = requests[0].fd();

  // Close the source fd
  handler.closeSourceFd(clientFd);

  // Verify it was closed
  CHECK(std::find(networkHandler->closedFds.begin(),
                  networkHandler->closedFds.end(),
                  clientFd) != networkHandler->closedFds.end());
}

TEST_CASE("PortForwardHandler addSourceSocketId", "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  // Create a source
  PortForwardSourceRequest sourceRequest;
  SocketEndpoint source;
  source.set_port(8080);
  *sourceRequest.mutable_source() = source;
  SocketEndpoint destination;
  destination.set_port(9090);
  *sourceRequest.mutable_destination() = destination;
  handler.createSource(sourceRequest, nullptr, 1000, 1000);

  // Accept a connection
  vector<PortForwardDestinationRequest> requests;
  vector<PortForwardData> dataToSend;

  auto fds = networkHandler->getEndpointFds(source);
  REQUIRE_FALSE(fds.empty());
  int listenFd = *(fds.begin());

  networkHandler->queueAccept(listenFd, 123);
  handler.update(&requests, &dataToSend);

  REQUIRE(requests.size() == 1);
  int clientFd = requests[0].fd();

  // Add socket ID mapping
  handler.addSourceSocketId(456, clientFd);

  // Verify we can send data to it
  handler.sendDataToSourceOnSocket(456, "test");
  REQUIRE(networkHandler->writes.count(clientFd) == 1);
  CHECK(networkHandler->writes[clientFd][0] == "test");
}

TEST_CASE("PortForwardHandler closeSourceSocketId", "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  // Create a source
  PortForwardSourceRequest sourceRequest;
  SocketEndpoint source;
  source.set_port(8080);
  *sourceRequest.mutable_source() = source;
  SocketEndpoint destination;
  destination.set_port(9090);
  *sourceRequest.mutable_destination() = destination;
  handler.createSource(sourceRequest, nullptr, 1000, 1000);

  // Accept and map a connection
  vector<PortForwardDestinationRequest> requests;
  vector<PortForwardData> dataToSend;

  auto fds = networkHandler->getEndpointFds(source);
  REQUIRE_FALSE(fds.empty());
  int listenFd = *(fds.begin());

  networkHandler->queueAccept(listenFd, 123);
  handler.update(&requests, &dataToSend);

  REQUIRE(requests.size() == 1);
  int clientFd = requests[0].fd();
  handler.addSourceSocketId(456, clientFd);

  // Close by socket ID
  handler.closeSourceSocketId(456);

  // Verify it was closed
  CHECK(std::find(networkHandler->closedFds.begin(),
                  networkHandler->closedFds.end(),
                  clientFd) != networkHandler->closedFds.end());
}

TEST_CASE("PortForwardHandler sendDataToSourceOnSocket",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  // Create a source
  PortForwardSourceRequest sourceRequest;
  SocketEndpoint source;
  source.set_port(8080);
  *sourceRequest.mutable_source() = source;
  SocketEndpoint destination;
  destination.set_port(9090);
  *sourceRequest.mutable_destination() = destination;
  handler.createSource(sourceRequest, nullptr, 1000, 1000);

  // Accept and map a connection
  vector<PortForwardDestinationRequest> requests;
  vector<PortForwardData> dataToSend;

  auto fds = networkHandler->getEndpointFds(source);
  REQUIRE_FALSE(fds.empty());
  int listenFd = *(fds.begin());

  networkHandler->queueAccept(listenFd, 123);
  handler.update(&requests, &dataToSend);

  REQUIRE(requests.size() == 1);
  int clientFd = requests[0].fd();
  handler.addSourceSocketId(456, clientFd);

  // Send data
  handler.sendDataToSourceOnSocket(456, "hello world");

  // Verify data was written
  REQUIRE(networkHandler->writes.count(clientFd) == 1);
  CHECK(networkHandler->writes[clientFd][0] == "hello world");
}
