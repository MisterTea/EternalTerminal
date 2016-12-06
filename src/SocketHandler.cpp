#include "SocketHandler.hpp"

ssize_t SocketHandler::readAll(int fd, void* buf, size_t count) {
  size_t pos=0;
  while (pos<count) {
    ssize_t bytesRead = read(fd, ((char*)buf) + pos, count - pos);
    if (bytesRead < 0) {
      VLOG(1) << "Failed a call to readAll: " << strerror(errno);
      throw std::runtime_error("Failed a call to readAll");
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
      VLOG(1) << "Failed a call to writeAll: " << strerror(errno);
      throw std::runtime_error("Failed a call to writeAll");
    }
    pos += bytesWritten;
  }
  return count;
}

#define SOCKET_TIMEOUT (60)

ssize_t SocketHandler::readAllTimeout(int fd, void* buf, size_t count) {
  time_t timeout = time(NULL) + SOCKET_TIMEOUT;
  size_t pos=0;
  while (pos<count) {
    ssize_t bytesRead = read(fd, ((char*)buf) + pos, count - pos);
    if (bytesRead < 0) {
      VLOG(1) << "Failed a call to readAll: " << strerror(errno);
      throw std::runtime_error("Failed a call to readAll");
    }
    VLOG(1) << "Read " << bytesRead << " bytes...";
    pos += bytesRead;
    if (timeout < time(NULL)) {
      throw runtime_error("Timeout");
    }
  }
  return count;
}

ssize_t SocketHandler::writeAllTimeout(int fd, const void* buf, size_t count) {
  time_t timeout = time(NULL) + SOCKET_TIMEOUT;
  size_t pos=0;
  while (pos<count) {
    ssize_t bytesWritten = write(fd, ((const char*)buf) + pos, count - pos);
    if (bytesWritten < 0) {
      VLOG(1) << "Failed a call to writeAll: " << strerror(errno);
      throw std::runtime_error("Failed a call to writeAll");
    }
    VLOG(1) << "Written " << bytesWritten << " bytes...";
    pos += bytesWritten;
    if (timeout < time(NULL)) {
      throw runtime_error("Timeout");
    }
  }
  return count;
}
