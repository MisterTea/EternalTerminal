#include "SocketHandler.hpp"

void SocketHandler::readAll(int fd, void* buf, size_t count) {
  size_t pos=0;
  while (pos<count) {
    ssize_t bytesRead = read(fd, ((char*)buf) + pos, count - pos);
    if (bytesRead < 0) {
      VLOG(1) << "Failed a call to readAll: " << strerror(errno);
      throw std::runtime_error("Failed a call to readAll");
    }
    pos += bytesRead;
  }
}

void SocketHandler::writeAll(int fd, const void* buf, size_t count) {
  size_t pos=0;
  while (pos<count) {
    ssize_t bytesWritten = write(fd, ((const char*)buf) + pos, count - pos);
    if (bytesWritten < 0) {
      VLOG(1) << "Failed a call to writeAll: " << strerror(errno);
      throw std::runtime_error("Failed a call to writeAll");
    }
    pos += bytesWritten;
  }
}
