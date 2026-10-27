#include "RawSocketUtils.hpp"

namespace et {
ssize_t RawSocketUtils::readSome(int fd, char* buf, size_t count) {
  return ::read(fd, buf, count);
}

ssize_t RawSocketUtils::writeSome(int fd, const char* buf, size_t count) {
  return ::write(fd, buf, count);
}

int RawSocketUtils::closeSocket(int fd) { return ::close(fd); }
void RawSocketUtils::unlinkSocketPath(const string& path) {
  ::unlink(path.c_str());
}
}  // namespace et
