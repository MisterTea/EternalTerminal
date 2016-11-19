#ifndef __ETERNAL_TCP_UNIX_SOCKET_HANDLER__
#define __ETERNAL_TCP_UNIX_SOCKET_HANDLER__

#include "SocketHandler.hpp"

class UnixSocketHandler : public SocketHandler {
public:
  virtual ssize_t read(int fd, void* buf, size_t count);
  virtual ssize_t write(int fd, void* buf, size_t count);
};

#endif // __ETERNAL_TCP_UNIX_SOCKET_HANDLER__
