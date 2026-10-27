#include "RawSocketUtils.hpp"

namespace et {
void RawSocketUtils::writeAll(int fd, const char* buf, size_t count) {
  if (fd < 0) {
    throw std::runtime_error("Invalid file descriptor for readAll");
  }
  if (count == 0) {
    return;
  }

  size_t bytesWritten = 0;
  do {
    const ssize_t rc = writeSome(fd, buf + bytesWritten, count - bytesWritten);
    if (rc < 0) {
      auto localErrno = GetErrno();
      if (localErrno == EAGAIN || localErrno == EWOULDBLOCK) {
        // This is fine, just keep retrying
        std::this_thread::sleep_for(std::chrono::microseconds(100 * 1000));
        continue;
      }
      STERROR << "Cannot write to raw socket: " << strerror(localErrno);
      throw std::runtime_error("Cannot write to raw socket");
    }
    if (rc == 0) {
      throw std::runtime_error("Cannot write to raw socket: socket closed");
    }
    bytesWritten += rc;
  } while (bytesWritten != count);
}

void RawSocketUtils::readAll(int fd, char* buf, size_t count) {
  if (fd < 0) {
    throw std::runtime_error("Invalid file descriptor for readAll");
  }
  if (count == 0) {
    return;
  }

  size_t bytesRead = 0;
  do {
    if (!waitOnSocketData(fd)) {
      continue;
    }
    const ssize_t rc = readSome(fd, buf + bytesRead, count - bytesRead);
    if (rc < 0) {
      auto localErrno = GetErrno();
      if (localErrno == EAGAIN || localErrno == EWOULDBLOCK) {
        // This is fine, just keep retrying
        continue;
      }
      STERROR << "Cannot write to raw socket: " << strerror(localErrno);
      throw std::runtime_error("Cannot read from raw socket");
    }
    if (rc == 0) {
      throw std::runtime_error("Socket has closed abruptly.");
    }
    bytesRead += rc;
  } while (bytesRead != count);
}

}  // namespace et
