#include "ForwardDestinationHandler.hpp"
#include "ForwardSourceHandler.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {
class FakeForwardSocketHandler : public SocketHandler {
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

  int connect(const SocketEndpoint& /*endpoint*/) override { return -1; }

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
  int nextListenFd = 100;
};
}  // namespace

TEST_CASE("ForwardDestinationHandler processes data and closure", "[integration]") {
  auto socketHandler = make_shared<FakeForwardSocketHandler>();
  const int fd = 5;
  const int socketId = 42;
  socketHandler->queueRead(fd, 4, "ping");
  socketHandler->queueRead(fd, 0);

  ForwardDestinationHandler destinationHandler(socketHandler, fd, socketId);
  vector<PortForwardData> forwarded;
  destinationHandler.update(&forwarded);

  REQUIRE(forwarded.size() == 2);
  CHECK(forwarded[0].socketid() == socketId);
  CHECK_FALSE(forwarded[0].sourcetodestination());
  CHECK(forwarded[0].buffer() == "ping");
  CHECK(forwarded[1].closed());
  CHECK(destinationHandler.getFd() == -1);
  REQUIRE(socketHandler->closedFds.size() == 1);
  CHECK(socketHandler->closedFds[0] == fd);
}

TEST_CASE("ForwardDestinationHandler stops when socket would block", "[integration]") {
  auto socketHandler = make_shared<FakeForwardSocketHandler>();
  const int fd = 6;
  socketHandler->queueRead(fd, -1, "", EAGAIN);

  ForwardDestinationHandler destinationHandler(socketHandler, fd, 7);
  vector<PortForwardData> forwarded;
  destinationHandler.update(&forwarded);

  CHECK(forwarded.empty());
  CHECK(destinationHandler.getFd() == fd);
  CHECK(socketHandler->closedFds.empty());
}

TEST_CASE("ForwardSourceHandler accepts, assigns, and routes data", "[integration]") {
  auto socketHandler = make_shared<FakeForwardSocketHandler>();
  SocketEndpoint sourceEndpoint;
  sourceEndpoint.set_name("source.sock");
  SocketEndpoint destinationEndpoint;
  destinationEndpoint.set_name("destination.sock");
  int listenFd = -1;
  int mappedFd = -1;

  {
    ForwardSourceHandler sourceHandler(socketHandler, sourceEndpoint,
                                      destinationEndpoint);
    auto fds = socketHandler->getEndpointFds(sourceEndpoint);
    REQUIRE_FALSE(fds.empty());
    listenFd = *(fds.begin());
    socketHandler->queueAccept(listenFd, 21);
    socketHandler->queueAccept(listenFd, 31);
    socketHandler->queueAccept(listenFd, 41);

    const int unassignedFd = sourceHandler.listen();
    REQUIRE(unassignedFd == 21);
    CHECK(sourceHandler.hasUnassignedFd(unassignedFd));

    sourceHandler.addSocket(9, unassignedFd);
    CHECK_FALSE(sourceHandler.hasUnassignedFd(unassignedFd));

    socketHandler->queueRead(unassignedFd, 3, "hey");
    socketHandler->queueRead(unassignedFd, 0);

    vector<PortForwardData> forwarded;
    sourceHandler.update(&forwarded);
    REQUIRE(forwarded.size() == 2);
    CHECK(forwarded[0].sourcetodestination());
    CHECK(forwarded[0].socketid() == 9);
    CHECK(forwarded[0].buffer() == "hey");
    CHECK(forwarded[1].closed());
    CHECK(std::find(socketHandler->closedFds.begin(),
                    socketHandler->closedFds.end(),
                    unassignedFd) != socketHandler->closedFds.end());

    const int stillUnassigned = sourceHandler.listen();
    REQUIRE(stillUnassigned == 31);
    sourceHandler.closeUnassignedFd(stillUnassigned);
    CHECK(std::find(socketHandler->closedFds.begin(),
                    socketHandler->closedFds.end(),
                    stillUnassigned) != socketHandler->closedFds.end());

    mappedFd = sourceHandler.listen();
    REQUIRE(mappedFd == 41);
    sourceHandler.addSocket(7, mappedFd);
    sourceHandler.sendDataOnSocket(7, "payload");
    sourceHandler.closeSocket(7);
  }

  REQUIRE(socketHandler->stoppedEndpoints.size() == 1);
  CHECK(socketHandler->stoppedEndpoints[0].name() == sourceEndpoint.name());
  CHECK(std::find(socketHandler->closedFds.begin(),
                  socketHandler->closedFds.end(),
                  mappedFd) != socketHandler->closedFds.end());
  REQUIRE(socketHandler->writes.count(mappedFd) == 1);
  CHECK(socketHandler->writes[mappedFd].back() == "payload");
}
