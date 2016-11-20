#include "SocketHandler.hpp"

ssize_t SocketHandler::readAll(int fd, void* buf, size_t count) {
  size_t pos=0;
  while (pos<count) {
    ssize_t bytesRead = read(fd, ((char*)buf) + pos, count - pos);
    if (bytesRead < 0) {
      fprintf(stderr, "Failed a call to readAll: %s\n", strerror(errno));
      throw new std::runtime_error("Failed a call to readAll");
    }
    pos += bytesRead;
  }
  return count;
}

ssize_t SocketHandler::writeAll(int fd, const void* buf, size_t count) {
  size_t pos=0;
  while (pos<count) {
    ssize_t bytesWritten = write(fd, ((const char*)buf) + pos, count - pos);
    if (bytesWritten < 0) {
      fprintf(stderr, "Failed a call to writeAll: %s\n", strerror(errno));
      throw new std::runtime_error("Failed a call to writeAll");
    }
    pos += bytesWritten;
  }
  return count;
}
