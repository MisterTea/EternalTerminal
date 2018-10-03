#ifndef __ET_FLAKY_SOCKET_HANDLER__
#define __ET_FLAKY_SOCKET_HANDLER__

#include "UnixSocketHandler.hpp"

namespace et {
class FlakySocketHandler : public SocketHandler {
 public:
  FlakySocketHandler(shared_ptr<SocketHandler> _actualSocketHandler)
      : actualSocketHandler(_actualSocketHandler) {}
  virtual ~FlakySocketHandler() {}

  virtual int connect(const SocketEndpoint& endpoint) {
    if (rand() % 2 == 0) {
      return -1;
    }
    return actualSocketHandler->connect(endpoint);
  }
  virtual set<int> listen(const SocketEndpoint& endpoint) {
    return actualSocketHandler->listen(endpoint);
  }
  virtual set<int> getEndpointFds(const SocketEndpoint& endpoint) {
    return actualSocketHandler->getEndpointFds(endpoint);
  }
  virtual void stopListening(const SocketEndpoint& endpoint) {
    return actualSocketHandler->stopListening(endpoint);
  }
  virtual bool hasData(int fd) {
    if (rand() % 10 == 0) {
      return false;
    }
    return actualSocketHandler->hasData(fd);
  }
  virtual ssize_t read(int fd, void* buf, size_t count) {
    if (rand() % 20 == 0) {
      errno=EPIPE;
      return -1;
    }
    return actualSocketHandler->read(fd, buf, count);
  }
  virtual ssize_t write(int fd, const void* buf, size_t count) {
    if (rand() % 20 == 0) {
      errno=EPIPE;
      return -1;
    }
    return actualSocketHandler->write(fd, buf, count);
  }
  virtual int accept(int fd) {
    if (rand() % 2 == 0) {
      errno=EAGAIN;
      return -1;
    }
    return actualSocketHandler->accept(fd);
  }
  virtual void close(int fd) { actualSocketHandler->close(fd); }

 protected:
  shared_ptr<SocketHandler> actualSocketHandler;
};
}  // namespace et

#endif  // __ET_FLAKY_SOCKET_HANDLER__
