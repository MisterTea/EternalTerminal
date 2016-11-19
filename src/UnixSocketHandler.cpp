#include "UnixSocketHandler.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

ssize_t UnixSocketHandler::read(int fd, void* buf, size_t count) {
  return ::read(fd, buf, count);
}

ssize_t UnixSocketHandler::write(int fd, void* buf, size_t count) {
  return ::write(fd, buf, count);
}
