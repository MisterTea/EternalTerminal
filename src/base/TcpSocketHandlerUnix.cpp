#include "TcpSocketHandler.hpp"

namespace et {
void TcpSocketHandler::refreshResolver() { ::res_init(); }

const char* TcpSocketHandler::addressError(int error) {
  return ::gai_strerror(error);
}

void TcpSocketHandler::minimizeKernelBuffering(int fd) {
#ifdef TCP_NOTSENT_LOWAT
  // Keep the kernel's not-yet-sent queue small so pending terminal output
  // stays in WriteBuffer, where Ctrl+C can drop it. Without this,
  // select()/poll() report the socket writable whenever the autotuned
  // (multi-MB) send buffer has room, and stale output piles up in the
  // kernel where it cannot be discarded. TCP_NOTSENT_LOWAT only limits
  // *unsent* data; sent-but-unacked (in-flight) data is unaffected.
  int lowat = 32 * 1024;
  if (setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, (char*)&lowat,
                 sizeof(lowat)) < 0) {
    LOG(WARNING) << "Failed to set TCP_NOTSENT_LOWAT: " << strerror(errno);
  }
#endif
}
}  // namespace et
