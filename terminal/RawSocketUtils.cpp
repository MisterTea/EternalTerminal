#include "RawSocketUtils.hpp"

namespace et {
int RawSocketUtils::writeAll(int fd, const char* buf, size_t count) {
  size_t bytesWritten = 0;
  do {
    int rc = write(fd, buf + bytesWritten, count - bytesWritten);
    if (rc < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // This is fine, just keep retrying at 10hz
        usleep(1000 * 100);
        continue;
      }
      // return rc;
      throw std::runtime_error("Cannot write to raw socket");
    }
    if (rc == 0) {
      LOG(ERROR) << "Could not write byte, trying again...";
    }
    bytesWritten += rc;
  } while (bytesWritten != count);
  return 0;
}

int RawSocketUtils::readAll(int fd, char* buf, size_t count) {
  size_t bytesRead = 0;
  do {
    int rc = ::read(fd, buf + bytesRead, count - bytesRead);
    if (rc < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // This is fine, just keep retrying at 10hz
        usleep(1000 * 100);
        continue;
      }
      // return rc;
      throw std::runtime_error("Cannot read from raw socket");
    }
    if (rc == 0) {
      throw std::runtime_error("Socket has closed abruptly.");
    }
    bytesRead += rc;
  } while (bytesRead != count);
  return bytesRead;
}
}  // namespace et
