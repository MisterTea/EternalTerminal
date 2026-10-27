#include <climits>

#include "RawSocketUtils.hpp"

namespace et {
ssize_t RawSocketUtils::readSome(int fd, char* buf, size_t count) {
  return ::recv(fd, buf, static_cast<int>(std::min<size_t>(count, INT_MAX)), 0);
}

ssize_t RawSocketUtils::writeSome(int fd, const char* buf, size_t count) {
  return ::send(fd, buf, static_cast<int>(std::min<size_t>(count, INT_MAX)), 0);
}

int RawSocketUtils::closeSocket(int fd) { return ::closesocket(fd); }
void RawSocketUtils::unlinkSocketPath(const string& path) {
  ::DeleteFileA(path.c_str());
}
}  // namespace et
