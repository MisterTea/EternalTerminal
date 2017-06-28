#include "SocketHandler.hpp"

namespace et {
#define SOCKET_DATA_TRANSFER_TIMEOUT (10)

void SocketHandler::readAll(int fd, void* buf, size_t count, bool timeout) {
  time_t startTime = time(NULL);
  size_t pos = 0;
  while (pos < count) {
    time_t currentTime = time(NULL);
    if (timeout && currentTime > startTime + 10) {
      throw std::runtime_error("Socket Timeout");
    }
    ssize_t bytesRead = read(fd, ((char*)buf) + pos, count - pos);
    if (bytesRead == 0) {
      // Connection is closed.  Instead of closing the socket, set EPIPE.
      // In EternalTCP, the server needs to explictly tell the client that
      // the session is over.
      errno = EPIPE;
      bytesRead = -1;
    }
    if (bytesRead < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // This is fine, just keep retrying at 10hz
        usleep(1000 * 100);
      } else {
        VLOG(1) << "Failed a call to readAll: " << strerror(errno);
        throw std::runtime_error("Failed a call to readAll");
      }
    } else {
      pos += bytesRead;
      if (bytesRead > 0) {
        // Reset the timeout as long as we are reading bytes
        startTime = currentTime;
      }
    }
  }
}

void SocketHandler::writeAll(int fd, const void* buf, size_t count,
                             bool timeout) {
  time_t startTime = time(NULL);
  size_t pos = 0;
  while (pos < count) {
    time_t currentTime = time(NULL);
    if (timeout && currentTime > startTime + 10) {
      throw std::runtime_error("Socket Timeout");
    }
    ssize_t bytesWritten = write(fd, ((const char*)buf) + pos, count - pos);
    if (bytesWritten < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // This is fine, just keep retrying at 10hz
        usleep(1000 * 100);
      } else {
        VLOG(1) << "Failed a call to writeAll: " << strerror(errno);
        throw std::runtime_error("Failed a call to writeAll");
      }
    } else {
      pos += bytesWritten;
      if (bytesWritten > 0) {
        // Reset the timeout as long as we are writing bytes
        startTime = currentTime;
      }
    }
  }
}
}
