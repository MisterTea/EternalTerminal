#include "RawSocketUtils.hpp"
#include "UnixSocketHandler.hpp"

namespace et {
void UnixSocketHandler::initSocket(int fd) {
  // Ignore SIGPIPE for socket writes on platforms without MSG_NOSIGNAL.
  ::signal(SIGPIPE, SIG_IGN);
  setBlocking(fd, false);
}
ssize_t UnixSocketHandler::writeSocketSome(int fd, const char* buf,
                                           size_t count) {
#ifdef MSG_NOSIGNAL
  return ::send(fd, buf, count, MSG_NOSIGNAL);
#else
  return RawSocketUtils::writeSome(fd, buf, count);
#endif
}
}  // namespace et
