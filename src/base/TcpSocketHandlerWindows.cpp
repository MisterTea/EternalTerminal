#include "TcpSocketHandler.hpp"

namespace et {
void TcpSocketHandler::refreshResolver() {}

const char* TcpSocketHandler::addressError(int error) {
  return ::gai_strerrorA(error);
}

void TcpSocketHandler::minimizeKernelBuffering(int /*fd*/) {}
}  // namespace et
