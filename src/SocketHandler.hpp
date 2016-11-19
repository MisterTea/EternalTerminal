#ifndef __ETERNAL_TCP_SOCKET_HANDLER__
#define __ETERNAL_TCP_SOCKET_HANDLER__

#include "Headers.hpp"

class SocketHandler {
public:
  virtual ssize_t read(int fd, void* buf, size_t count) = 0;
  virtual ssize_t write(int fd, const void* buf, size_t count) = 0;
};

#endif // __ETERNAL_TCP_SOCKET_HANDLER__
