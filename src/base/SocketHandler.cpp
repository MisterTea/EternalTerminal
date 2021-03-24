#include "SocketHandler.hpp"

#include "base64.h"

namespace et {
#define SOCKET_DATA_TRANSFER_TIMEOUT (10)

void SocketHandler::readAll(int fd, void* buf, size_t count, bool timeout) {
  time_t startTime = time(NULL);
  size_t pos = 0;
  while (pos < count) {
    if (!waitOnSocketData(fd)) {
      time_t currentTime = time(NULL);
      if (timeout && currentTime > startTime + 10) {
        throw std::runtime_error("Socket Timeout");
      }
      continue;
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
      auto localErrno = errno;
      if (localErrno == EAGAIN || localErrno == EWOULDBLOCK) {
        // This is fine, just keep retrying
        LOG(INFO) << "Got EAGAIN, waiting...";
      } else {
        VLOG(1) << "Failed a call to readAll: " << strerror(localErrno);
        throw std::runtime_error("Failed a call to readAll");
      }
    } else {
      pos += bytesRead;
      startTime = time(NULL);
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
    auto localErrno = errno;
    if (bytesWritten < 0) {
      if (localErrno == EAGAIN || localErrno == EWOULDBLOCK) {
        LOG(INFO) << "Got EAGAIN, waiting...";
        // This is fine, just keep retrying at 10hz
        std::this_thread::sleep_for(std::chrono::microseconds(100 * 1000));
      } else {
        VLOG(1) << "Failed a call to writeAll: " << strerror(localErrno);
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
    auto localErrno = errno;
    if (bytesWritten < 0) {
      if (localErrno == EAGAIN || localErrno == EWOULDBLOCK) {
        LOG(INFO) << "Got EAGAIN, waiting...";
        // This is fine, just keep retrying at 10hz
        std::this_thread::sleep_for(std::chrono::microseconds(100 * 1000));
      } else {
        LOG(WARNING) << "Failed a call to writeAll: " << strerror(localErrno);
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

void SocketHandler::writeB64(int fd, const char* buf, size_t count) {
  size_t encodedLength = Base64::EncodedLength(count);
  string s(encodedLength, '\0');
  if (!Base64::Encode(buf, count, &s[0], s.length())) {
    throw runtime_error("b64 decode failed");
  }
  writeAllOrThrow(fd, &s[0], s.length(), false);
}

void SocketHandler::readB64(int fd, char* buf, size_t count) {
  size_t encodedLength = Base64::EncodedLength(count);
  string s(encodedLength, '\0');
  readAll(fd, &s[0], s.length(), false);
  if (!Base64::Decode((const char*)&s[0], s.length(), buf, count)) {
    throw runtime_error("b64 decode failed");
  }
}

void SocketHandler::readB64EncodedLength(int fd, string* out, size_t encodedLength) {
  string s(encodedLength, '\0');
  readAll(fd, &s[0], s.length(), false);
  if (!Base64::Decode(s, out)) {
    throw runtime_error("b64 decode failed");
  }
}
}  // namespace et
