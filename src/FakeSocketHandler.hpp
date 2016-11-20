#ifndef __ETERNAL_TCP_FAKE_SOCKET_HANDLER__
#define __ETERNAL_TCP_FAKE_SOCKET_HANDLER__

#include "SocketHandler.hpp"

class FakeSocketHandler : public SocketHandler {
public:
  explicit FakeSocketHandler();

  explicit FakeSocketHandler(std::shared_ptr<FakeSocketHandler> remoteHandler);

  inline void setRemoteHandler(std::shared_ptr<FakeSocketHandler> remoteHandler) {
    this->remoteHandler = remoteHandler;
  }

  virtual ssize_t read(int fd, void* buf, size_t count);
  virtual ssize_t write(int fd, const void* buf, size_t count);

  void push(const char* buf, size_t count);
protected:

  std::shared_ptr<FakeSocketHandler> remoteHandler;
  std::string inBuffer;
  boost::mutex inBufferMutex;
};

#endif // __ETERNAL_TCP_FAKE_SOCKET_HANDLER__
