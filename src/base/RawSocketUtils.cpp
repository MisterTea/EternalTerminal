#include "RawSocketUtils.hpp"

namespace et {
#ifdef WIN32
void RawSocketUtils::writeAll(HANDLE fd, const char* buf, size_t count) {
  size_t bytesWritten = 0;
  do {
    DWORD newBytesWritten;
    FATAL_FAIL_IF_ZERO(WriteFile(fd, buf + bytesWritten, count - bytesWritten,
                                 &newBytesWritten, NULL));
    bytesWritten += newBytesWritten;
  } while (bytesWritten != count);
}

void RawSocketUtils::readAll(HANDLE fd, char* buf, size_t count) {
  size_t bytesRead = 0;
  do {
    DWORD newBytesRead;
    FATAL_FAIL_IF_ZERO(
        ReadFile(fd, buf + bytesRead, count - bytesRead, &newBytesRead, NULL));
    bytesRead += newBytesRead;
  } while (bytesRead != count);
}
#else
void RawSocketUtils::writeAll(int fd, const char* buf, size_t count) {
  size_t bytesWritten = 0;
  do {
#ifdef WIN32
    int rc = ::send(fd, buf + bytesWritten, count - bytesWritten, 0);
#else
    int rc = ::write(fd, buf + bytesWritten, count - bytesWritten);
#endif
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
  size_t bytesRead = 0;
  do {
    if (!waitOnSocketData(fd)) {
      continue;
    }
#ifdef WIN32
    int rc = ::recv(fd, buf + bytesRead, count - bytesRead, 0);
#else
    int rc = ::read(fd, buf + bytesRead, count - bytesRead);
#endif
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
#endif
}  // namespace et
