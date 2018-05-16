#include "RawSocketUtils.hpp"

namespace et {
void RawSocketUtils::writeAll(int fd, const char* buf, size_t count) {
  size_t bytesWritten = 0;
  do {
    int rc = write(fd, buf + bytesWritten, count - bytesWritten);
    if (rc < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // This is fine, just keep retrying at 10hz
        usleep(1000 * 100);
        continue;
      }
      throw std::runtime_error("Cannot write to raw socket");
    }
    if (rc == 0) {
      throw std::runtime_error("Cannot write to raw socket: socket closed");
    }
    bytesWritten += rc;
  } while (bytesWritten != count);
}

void RawSocketUtils::readAll(int fd, char* buf, size_t count) {
  size_t bytesRead = 0;
  do {
    int rc = ::read(fd, buf + bytesRead, count - bytesRead);
    if (rc < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // This is fine, just keep retrying at 10hz
        usleep(1000 * 100);
        continue;
      }
      throw std::runtime_error("Cannot read from raw socket");
    }
    if (rc == 0) {
      throw std::runtime_error("Socket has closed abruptly.");
    }
    bytesRead += rc;
  } while (bytesRead != count);
}
}  // namespace et
