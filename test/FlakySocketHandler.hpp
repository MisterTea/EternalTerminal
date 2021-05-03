#ifndef __ET_FLAKY_SOCKET_HANDLER__
#define __ET_FLAKY_SOCKET_HANDLER__

#include "UnixSocketHandler.hpp"

namespace et {
class FlakySocketHandler : public SocketHandler {
 public:
  FlakySocketHandler(shared_ptr<SocketHandler> _actualSocketHandler,
                     bool _enableFlake)
      : actualSocketHandler(_actualSocketHandler), enableFlake(_enableFlake) {}
  virtual ~FlakySocketHandler() {}

  inline void setFlake(bool _enableFlake) { enableFlake = _enableFlake; }

  virtual int connect(const SocketEndpoint& endpoint) {
    if (enableFlake && rand() % 2 == 0) {
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
    if (enableFlake && rand() % 2 == 0) {
      return false;
    }
    return actualSocketHandler->hasData(fd);
  }
  virtual ssize_t read(int fd, void* buf, size_t count) {
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    if (enableFlake && millis % 10 == 0) {
      SetErrno(EPIPE);
      return -1;
    }
    if (enableFlake && millis % 10 == 5) {
      SetErrno(EAGAIN);
      return -1;
    }
    return actualSocketHandler->read(fd, buf, count);
  }
  virtual ssize_t write(int fd, const void* buf, size_t count) {
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    if (enableFlake && millis % 10 == 0) {
      SetErrno(EPIPE);
      return -1;
    }
    if (enableFlake && millis % 10 == 5) {
      SetErrno(EAGAIN);
      return -1;
    }
    return actualSocketHandler->write(fd, buf, count);
  }
  virtual vector<int> getActiveSockets() {
    return actualSocketHandler->getActiveSockets();
  }

  int writeAllOrReturn(int fd, const void* buf, size_t count) {
    if (enableFlake && rand() % 30 == 0) {
      SetErrno(EPIPE);
      return -1;
    }
    return actualSocketHandler->writeAllOrReturn(fd, buf, count);
  }

  virtual int accept(int fd) {
    if (enableFlake && rand() % 2 == 0) {
      SetErrno(EAGAIN);
      return -1;
    }
    return actualSocketHandler->accept(fd);
  }
  virtual void close(int fd) { actualSocketHandler->close(fd); }

 protected:
  shared_ptr<SocketHandler> actualSocketHandler;
  bool enableFlake;
};
}  // namespace et

#endif  // __ET_FLAKY_SOCKET_HANDLER__
