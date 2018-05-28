#ifndef __ETERNAL_TCP_FAKE_SOCKET_HANDLER__
#define __ETERNAL_TCP_FAKE_SOCKET_HANDLER__

#include "SocketHandler.hpp"
namespace et {

class FakeSocketHandler : public SocketHandler {
 public:
  FakeSocketHandler();

  explicit FakeSocketHandler(std::shared_ptr<FakeSocketHandler> remoteHandler);

  inline void setRemoteHandler(
      std::shared_ptr<FakeSocketHandler> remoteHandler) {
    this->remoteHandler = remoteHandler;
  }

  virtual bool hasData(int fd);
  virtual ssize_t read(int fd, void* buf, size_t count);
  virtual ssize_t write(int fd, const void* buf, size_t count);
  virtual int connect(const std::string& hostname, int port);
  virtual void listen(int port);
  inline virtual set<int> getPortFds(int port) {
    set<int> s;
    return s;
  }
  virtual int accept(int fd);
  virtual void stopListening(int port);
  virtual void close(int fd);

  void push(int fd, const char* buf, size_t count);
  void addConnection(int fd);
  bool hasPendingConnection();

  int fakeConnection();

 protected:
  std::shared_ptr<FakeSocketHandler> remoteHandler;
  unordered_map<int, std::string> inBuffers;
  unordered_set<int> closedFds;
  mutex handlerMutex;
  int nextFd;
  vector<int> futureConnections;
};
}  // namespace et

#endif  // __ETERNAL_TCP_FAKE_SOCKET_HANDLER__
