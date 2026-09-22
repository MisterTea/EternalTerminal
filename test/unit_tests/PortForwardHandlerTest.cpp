#include "PipeSocketHandler.hpp"
#include "PortForwardHandler.hpp"
#include "TestHeaders.hpp"

#ifndef WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

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

  void setConnectResult(int fd) { connectResults = {fd}; }

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
    if (connectResults.empty()) {
      return -1;
    }
    int fd = connectResults.front();
    connectResults.pop_front();
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
  std::deque<int> connectResults;
};

class FakeConnection : public Connection {
 public:
  FakeConnection() : Connection(nullptr, "", "") {}
  ~FakeConnection() override { shutdown(); }

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

#ifndef WIN32
TEST_CASE("PortForwardHandler removes its generated socket directory",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<PipeSocketHandler>();
  string sourceName;
  {
    PortForwardHandler handler(networkHandler, pipeHandler);
    PortForwardSourceRequest request;
    request.mutable_destination()->set_name("/unused/destination");
    auto response =
        handler.createSource(request, &sourceName, getuid(), getgid());
    REQUIRE_FALSE(response.has_error());
    REQUIRE_FALSE(sourceName.empty());
    CHECK(fs::exists(sourceName));
  }
  CHECK_FALSE(fs::exists(sourceName));
  CHECK_FALSE(fs::exists(fs::path(sourceName).parent_path()));
}

TEST_CASE("PortForwardHandler preserves user-specified socket directories",
          "[PortForwardHandler]") {
  string pattern = GetTempDirectory() + "et_forward_test_XXXXXX";
  char* directory = mkdtemp(&pattern[0]);
  REQUIRE(directory != nullptr);
  const string sourceName = string(directory) + "/sock";
  {
    auto networkHandler = make_shared<FakePortForwardSocketHandler>();
    auto pipeHandler = make_shared<PipeSocketHandler>();
    PortForwardHandler handler(networkHandler, pipeHandler);
    PortForwardSourceRequest request;
    request.mutable_source()->set_name(sourceName);
    request.mutable_destination()->set_name("/unused/destination");
    auto response = handler.createSource(request, nullptr, getuid(), getgid());
    REQUIRE_FALSE(response.has_error());
  }
  CHECK(fs::exists(directory));
  fs::remove(sourceName);
  fs::remove(directory);
}

TEST_CASE("PortForwardHandler directory removal does not follow symlinks",
          "[PortForwardHandler]") {
  string pattern = GetTempDirectory() + "et_forward_test_XXXXXX";
  char* directory = mkdtemp(&pattern[0]);
  REQUIRE(directory != nullptr);
  const fs::path root(directory);
  const auto target = root / "target";
  const auto moved = root / "moved";
  fs::create_directory(target);
  {
    std::ofstream marker((target / "sock").string());
    marker << "preserve this file";
  }
  string sourceName;
  {
    auto networkHandler = make_shared<FakePortForwardSocketHandler>();
    auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
    PortForwardHandler handler(networkHandler, pipeHandler);
    PortForwardSourceRequest request;
    request.mutable_destination()->set_name("/unused/destination");
    auto response =
        handler.createSource(request, &sourceName, getuid(), getgid());
    REQUIRE_FALSE(response.has_error());
    auto sourceDirectory = fs::path(sourceName).parent_path();
    fs::rename(sourceDirectory, moved);
    fs::create_directory_symlink(target, sourceDirectory);
  }
  CHECK(fs::exists(target / "sock"));
  CHECK_FALSE(fs::exists(fs::path(sourceName).parent_path()));
  fs::remove(moved / "sock");
  fs::remove(moved);
  fs::remove(target / "sock");
  fs::remove(target);
  fs::remove(root);
}
#endif

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

TEST_CASE("PortForwardHandler empty TCP host uses loopback",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  networkHandler->setConnectResult(42);

  PortForwardDestinationRequest request;
  SocketEndpoint destination;
  destination.set_name("");
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

  bool ipv4Available = false;
  SECTION("IPv4 succeeds") {
    networkHandler->connectResults = {-1, 42};
    ipv4Available = true;
  }
  SECTION("Both addresses fail") { networkHandler->connectResults = {-1, -1}; }

  PortForwardDestinationRequest request;
  SocketEndpoint destination;
  destination.set_port(8080);
  *request.mutable_destination() = destination;
  request.set_fd(100);

  PortForwardDestinationResponse response = handler.createDestination(request);

  CHECK(response.clientfd() == 100);
  CHECK(response.has_error() == !ipv4Available);
  CHECK(response.has_socketid() == ipv4Available);
  REQUIRE(networkHandler->connectEndpoints.size() == 2);
  CHECK(networkHandler->connectEndpoints[0].name() == "::1");
  CHECK(networkHandler->connectEndpoints[1].name() == "127.0.0.1");
}

TEST_CASE("PortForwardHandler createDestination uses explicit TCP host",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  networkHandler->setConnectResult(42);

  PortForwardDestinationRequest request;
  SocketEndpoint destination;
  destination.set_name("destination.example.com");
  destination.set_port(22);
  *request.mutable_destination() = destination;
  request.set_fd(100);

  PortForwardDestinationResponse response = handler.createDestination(request);

  CHECK(response.clientfd() == 100);
  CHECK_FALSE(response.has_error());
  CHECK(response.has_socketid());
  REQUIRE(networkHandler->connectEndpoints.size() == 1);
  CHECK(networkHandler->connectEndpoints[0].name() ==
        "destination.example.com");
  CHECK(networkHandler->connectEndpoints[0].port() == 22);
}

TEST_CASE("PortForwardHandler does not replace a failed explicit TCP host",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  PortForwardDestinationRequest request;
  SocketEndpoint destination;
  destination.set_name("destination.example.com");
  destination.set_port(22);
  *request.mutable_destination() = destination;
  request.set_fd(100);

  PortForwardDestinationResponse response = handler.createDestination(request);

  CHECK(response.has_error());
  CHECK_FALSE(response.has_socketid());
  REQUIRE(networkHandler->connectEndpoints.size() == 1);
  CHECK(networkHandler->connectEndpoints[0].name() ==
        "destination.example.com");
  CHECK(networkHandler->connectEndpoints[0].port() == 22);
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

TEST_CASE("PortForwardHandler closes destinations on remote close or error",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);
  auto connection = make_shared<FakeConnection>();

  networkHandler->setConnectResult(42);
  PortForwardDestinationRequest destRequest;
  destRequest.mutable_destination()->set_port(8080);
  destRequest.set_fd(100);
  PortForwardDestinationResponse destResponse =
      handler.createDestination(destRequest);
  REQUIRE_FALSE(destResponse.has_error());
  PortForwardData data;
  data.set_sourcetodestination(true);
  data.set_socketid(destResponse.socketid());
  SECTION("Close") { data.set_closed(true); }
  SECTION("Error") { data.set_error("connection error"); }

  Packet packet(uint8_t(TerminalPacketType::PORT_FORWARD_DATA),
                protoToString(data));
  handler.handlePacket(packet, connection);

  CHECK(networkHandler->closedFds == vector<int>{42});
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

TEST_CASE("PortForwardHandler getForwardFds with no handlers",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  set<int> fds;
  handler.getForwardFds(&fds);

  CHECK(fds.empty());
}

TEST_CASE(
    "PortForwardHandler getForwardFds includes source and destination fds",
    "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  // Create a source (this creates a listener fd via listen())
  PortForwardSourceRequest sourceRequest;
  SocketEndpoint source;
  source.set_port(8080);
  *sourceRequest.mutable_source() = source;
  SocketEndpoint destination;
  destination.set_port(9090);
  *sourceRequest.mutable_destination() = destination;
  handler.createSource(sourceRequest, nullptr, 1000, 1000);

  // Create a destination
  networkHandler->setConnectResult(42);
  PortForwardDestinationRequest destRequest;
  SocketEndpoint dest2;
  dest2.set_port(3000);
  *destRequest.mutable_destination() = dest2;
  destRequest.set_fd(200);
  PortForwardDestinationResponse destResponse =
      handler.createDestination(destRequest);
  REQUIRE_FALSE(destResponse.has_error());

  set<int> fds;
  handler.getForwardFds(&fds);

  // Should include the source listener fd (assigned by
  // FakePortForwardSocketHandler)
  auto sourceFds = networkHandler->getEndpointFds(source);
  for (int fd : sourceFds) {
    CHECK(fds.count(fd) == 1);
  }

  // Should include the destination fd (42)
  CHECK(fds.count(42) == 1);
}

TEST_CASE("PortForwardHandler getForwardFds excludes closed destination fds",
          "[PortForwardHandler]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);
  auto connection = make_shared<FakeConnection>();

  // Create a destination
  networkHandler->setConnectResult(42);
  PortForwardDestinationRequest destRequest;
  SocketEndpoint dest;
  dest.set_port(3000);
  *destRequest.mutable_destination() = dest;
  destRequest.set_fd(200);
  PortForwardDestinationResponse destResponse =
      handler.createDestination(destRequest);
  REQUIRE_FALSE(destResponse.has_error());
  int socketId = destResponse.socketid();

  // Close the destination via a close packet
  PortForwardData data;
  data.set_sourcetodestination(true);
  data.set_socketid(socketId);
  data.set_closed(true);
  Packet packet(uint8_t(TerminalPacketType::PORT_FORWARD_DATA),
                protoToString(data));
  handler.handlePacket(packet, connection);

  set<int> fds;
  handler.getForwardFds(&fds);

  // Destination fd should be -1 after close, so not included
  CHECK(fds.count(42) == 0);
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

namespace {
string socks5AuthNoAuth() { return string("\x05\x01\x00", 3); }

string socks5ConnectIpv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                         uint16_t port) {
  string req("\x05\x01\x00\x01", 4);
  req.push_back(static_cast<char>(a));
  req.push_back(static_cast<char>(b));
  req.push_back(static_cast<char>(c));
  req.push_back(static_cast<char>(d));
  req.push_back(static_cast<char>((port >> 8) & 0xff));
  req.push_back(static_cast<char>(port & 0xff));
  return req;
}

string socks5ConnectDomain(const string& host, uint16_t port) {
  string req("\x05\x01\x00\x03", 4);
  req.push_back(static_cast<char>(host.size()));
  req += host;
  req.push_back(static_cast<char>((port >> 8) & 0xff));
  req.push_back(static_cast<char>(port & 0xff));
  return req;
}
}  // namespace

TEST_CASE("PortForwardHandler SOCKS -D chooses destination after connect",
          "[PortForwardHandler][runtime-forward]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  SocketEndpoint source;
  source.set_name("127.0.0.1");
  source.set_port(1080);
  REQUIRE_FALSE(handler.createSocksSource(source).has_error());

  auto listenFds = networkHandler->getEndpointFds(source);
  REQUIRE_FALSE(listenFds.empty());
  int listenFd = *listenFds.begin();
  networkHandler->queueAccept(listenFd, 200);
  networkHandler->queueRead(
      200, static_cast<int>(socks5AuthNoAuth().size()), socks5AuthNoAuth());
  networkHandler->queueRead(
      200, static_cast<int>(socks5ConnectIpv4(10, 0, 0, 2, 443).size()),
      socks5ConnectIpv4(10, 0, 0, 2, 443));

  vector<PortForwardDestinationRequest> requests;
  vector<PortForwardData> dataToSend;
  handler.update(&requests, &dataToSend);

  REQUIRE(requests.size() == 1);
  CHECK(requests[0].fd() == 200);
  CHECK(requests[0].destination().name() == "10.0.0.2");
  CHECK(requests[0].destination().port() == 443);
  REQUIRE(networkHandler->writes.count(200) == 1);
  // Auth reply + connect success reply were written to the SOCKS client.
  CHECK(networkHandler->writes[200][0].size() >= 2);
}

TEST_CASE("PortForwardHandler SOCKS concurrent channels",
          "[PortForwardHandler][runtime-forward]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  SocketEndpoint source;
  source.set_port(1080);
  REQUIRE_FALSE(handler.createSocksSource(source).has_error());

  auto listenFds = networkHandler->getEndpointFds(source);
  REQUIRE_FALSE(listenFds.empty());
  int listenFd = *listenFds.begin();

  networkHandler->queueAccept(listenFd, 201);
  networkHandler->queueAccept(listenFd, 202);
  auto req1 = socks5AuthNoAuth() + socks5ConnectIpv4(1, 2, 3, 4, 80);
  auto req2 = socks5AuthNoAuth() + socks5ConnectDomain("db.internal", 5432);
  networkHandler->queueRead(201, static_cast<int>(req1.size()), req1);
  networkHandler->queueRead(202, static_cast<int>(req2.size()), req2);

  vector<PortForwardDestinationRequest> requests;
  vector<PortForwardData> dataToSend;
  handler.update(&requests, &dataToSend);
  // First accept completes one SOCKS handshake; second accept needs another
  // update because listen() returns one ready fd at a time.
  handler.update(&requests, &dataToSend);

  REQUIRE(requests.size() == 2);
  CHECK(requests[0].destination().name() == "1.2.3.4");
  CHECK(requests[0].destination().port() == 80);
  CHECK(requests[1].destination().name() == "db.internal");
  CHECK(requests[1].destination().port() == 5432);

  handler.addSourceSocketId(11, requests[0].fd());
  handler.addSourceSocketId(22, requests[1].fd());
  networkHandler->queueRead(requests[0].fd(), 4, "one!");
  networkHandler->queueRead(requests[1].fd(), 4, "two!");

  dataToSend.clear();
  handler.update(&requests, &dataToSend);
  REQUIRE(dataToSend.size() >= 2);
  bool sawOne = false;
  bool sawTwo = false;
  for (const auto& pwd : dataToSend) {
    if (pwd.socketid() == 11 && pwd.buffer() == "one!") {
      sawOne = true;
    }
    if (pwd.socketid() == 22 && pwd.buffer() == "two!") {
      sawTwo = true;
    }
  }
  CHECK(sawOne);
  CHECK(sawTwo);

  handler.sendDataToSourceOnSocket(11, "A");
  handler.sendDataToSourceOnSocket(22, "B");
  CHECK(networkHandler->writes[requests[0].fd()].back() == "A");
  CHECK(networkHandler->writes[requests[1].fd()].back() == "B");
}

TEST_CASE("PortForwardHandler SOCKS unix destination via domain",
          "[PortForwardHandler][runtime-forward]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  SocketEndpoint source;
  source.set_port(1080);
  REQUIRE_FALSE(handler.createSocksSource(source).has_error());

  auto listenFds = networkHandler->getEndpointFds(source);
  int listenFd = *listenFds.begin();
  networkHandler->queueAccept(listenFd, 301);
  auto req =
      socks5AuthNoAuth() + socks5ConnectDomain("/var/run/docker.sock", 0);
  networkHandler->queueRead(301, static_cast<int>(req.size()), req);

  vector<PortForwardDestinationRequest> requests;
  vector<PortForwardData> dataToSend;
  handler.update(&requests, &dataToSend);

  REQUIRE(requests.size() == 1);
  CHECK(requests[0].destination().name() == "/var/run/docker.sock");
  CHECK_FALSE(requests[0].destination().has_port());
}

#ifndef WIN32
TEST_CASE("PortForwardHandler -W stdio byte forward without shell",
          "[PortForwardHandler][runtime-forward]") {
  auto networkHandler = make_shared<FakePortForwardSocketHandler>();
  auto pipeHandler = make_shared<FakePortForwardSocketHandler>();
  PortForwardHandler handler(networkHandler, pipeHandler);

  int inPipe[2];
  int outPipe[2];
  REQUIRE(pipe(inPipe) == 0);
  REQUIRE(pipe(outPipe) == 0);
  REQUIRE(fcntl(inPipe[0], F_SETFL, O_NONBLOCK) == 0);
  REQUIRE(fcntl(outPipe[1], F_SETFL, O_NONBLOCK) == 0);

  SocketEndpoint destination;
  destination.set_name("127.0.0.1");
  destination.set_port(9);
  REQUIRE_FALSE(
      handler.createStdioForward(destination, inPipe[0], outPipe[1], false)
          .has_error());
  CHECK(handler.hasActiveStdioForward());

  vector<PortForwardDestinationRequest> requests;
  vector<PortForwardData> dataToSend;
  handler.update(&requests, &dataToSend);

  REQUIRE(requests.size() == 1);
  CHECK(requests[0].fd() == inPipe[0]);
  CHECK(requests[0].destination().name() == "127.0.0.1");
  CHECK(requests[0].destination().port() == 9);

  handler.addSourceSocketId(77, inPipe[0]);
  const char payload[] = "stdio-bytes";
  REQUIRE(write(inPipe[1], payload, sizeof(payload) - 1) ==
          static_cast<ssize_t>(sizeof(payload) - 1));

  requests.clear();
  dataToSend.clear();
  handler.update(&requests, &dataToSend);
  REQUIRE(dataToSend.size() == 1);
  CHECK(dataToSend[0].socketid() == 77);
  CHECK(dataToSend[0].buffer() == "stdio-bytes");
  CHECK(dataToSend[0].sourcetodestination());

  handler.sendDataToSourceOnSocket(77, "from-remote");
  char buf[64];
  ssize_t n = read(outPipe[0], buf, sizeof(buf));
  REQUIRE(n == static_cast<ssize_t>(strlen("from-remote")));
  CHECK(string(buf, n) == "from-remote");

  // Closing the read side ends the stdio bridge (no shell involved).
  close(inPipe[1]);
  requests.clear();
  dataToSend.clear();
  handler.update(&requests, &dataToSend);
  REQUIRE_FALSE(dataToSend.empty());
  CHECK(dataToSend.back().closed());
  CHECK_FALSE(handler.hasActiveStdioForward());

  close(inPipe[0]);
  close(outPipe[0]);
  close(outPipe[1]);
}
#endif

