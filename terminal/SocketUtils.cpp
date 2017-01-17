#include "SocketUtils.hpp"

int writeAll(int fd, const char* buf, size_t count) {
  size_t bytesWritten = 0;
  do {
    int rc = write(fd, buf + bytesWritten, count - bytesWritten);
    if (rc < 0) {
      return rc;
    }
    if (rc == 0) {
      LOG(ERROR) << "Could not write byte, trying again...";
    }
    bytesWritten += rc;
  } while (bytesWritten != count);
  return 0;
}
