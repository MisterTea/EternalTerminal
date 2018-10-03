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
        usleep(100 * 1000);
        LOG(INFO) << "Got EAGAIN, waiting 100ms...";
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

int SocketHandler::writeAllOrReturn(int fd, const void* buf, size_t count) {
  size_t pos = 0;
  time_t startTime = time(NULL);
  while (pos < count) {
    time_t currentTime = time(NULL);
    if (currentTime > startTime + 10) {
      return -1;
    }
    ssize_t bytesWritten = write(fd, ((const char*)buf) + pos, count - pos);
    if (bytesWritten < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // This is fine, just keep retrying at 10hz
        usleep(100 * 1000);
        LOG(INFO) << "Got EAGAIN, waiting 100ms...";
      } else {
        VLOG(1) << "Failed a call to writeAll: " << strerror(errno);
        return -1;
      }
    } else if (bytesWritten == 0) {
      return 0;
    } else {
      pos += bytesWritten;
      // Reset the timeout as long as we are writing bytes
      startTime = currentTime;
    }
  }
  return count;
}

void SocketHandler::writeAllOrThrow(int fd, const void* buf, size_t count,
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
        usleep(100 * 1000);
        LOG(INFO) << "Got EAGAIN, waiting 100ms...";
      } else {
        VLOG(1) << "Failed a call to writeAll: " << strerror(errno);
        throw std::runtime_error("Failed a call to writeAll");
      }
    } else if (bytesWritten == 0) {
      throw std::runtime_error("Socket closed during writeAll");
    } else {
      pos += bytesWritten;
      // Reset the timeout as long as we are writing bytes
      startTime = currentTime;
    }
  }
}
}  // namespace et
