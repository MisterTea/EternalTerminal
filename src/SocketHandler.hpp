#ifndef __ETERNAL_TCP_SOCKET_HANDLER__
#define __ETERNAL_TCP_SOCKET_HANDLER__

#include "Headers.hpp"

class SocketHandler {
public:
  virtual bool hasData(int fd) = 0;
  virtual ssize_t read(int fd, void* buf, size_t count) = 0;
  virtual ssize_t write(int fd, const void* buf, size_t count) = 0;

  virtual ssize_t readAll(int fd, void* buf, size_t count);
  virtual ssize_t writeAll(int fd, const void* buf, size_t count);

  virtual ssize_t readAllTimeout(int fd, void* buf, size_t count);
  virtual ssize_t writeAllTimeout(int fd, const void* buf, size_t count);

  virtual int connect(const std::string &hostname, int port) = 0;
  virtual int listen(int port) = 0;
  virtual void stopListening() = 0;
  virtual void close(int fd) = 0;
};

#endif // __ETERNAL_TCP_SOCKET_HANDLER__
