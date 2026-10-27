#include "RawSocketUtils.hpp"
#include "UnixSocketHandler.hpp"

namespace et {
void UnixSocketHandler::initSocket(int fd) { setBlocking(fd, false); }
ssize_t UnixSocketHandler::writeSocketSome(int fd, const char* buf,
                                           size_t count) {
  return RawSocketUtils::writeSome(fd, buf, count);
}
}  // namespace et
