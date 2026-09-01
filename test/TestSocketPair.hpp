#ifndef __ET_TEST_SOCKET_PAIR_HPP__
#define __ET_TEST_SOCKET_PAIR_HPP__

#include "Headers.hpp"

namespace et {
namespace test {
// Bidirectional connected pair. Unix uses AF_UNIX socketpair; Windows has no
// socketpair, so tests fall back to a loopback TCP pair. A pipe is not a
// substitute: handshake tests write to both ends.
inline int createTestSocketPair(int sockets[2]) {
#ifndef WIN32
  return ::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
#else
  SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    return -1;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(listener, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) == SOCKET_ERROR ||
      ::listen(listener, 1) == SOCKET_ERROR) {
    ::closesocket(listener);
    return -1;
  }

  int addressLength = sizeof(address);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                    &addressLength) == SOCKET_ERROR) {
    ::closesocket(listener);
    return -1;
  }

  SOCKET client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (client == INVALID_SOCKET ||
      ::connect(client, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == SOCKET_ERROR) {
    if (client != INVALID_SOCKET) {
      ::closesocket(client);
    }
    ::closesocket(listener);
    return -1;
  }

  SOCKET server = ::accept(listener, nullptr, nullptr);
  ::closesocket(listener);
  if (server == INVALID_SOCKET || client > INT_MAX || server > INT_MAX) {
    if (server != INVALID_SOCKET) {
      ::closesocket(server);
    }
    ::closesocket(client);
    return -1;
  }
  sockets[0] = static_cast<int>(server);
  sockets[1] = static_cast<int>(client);
  return 0;
#endif
}
}  // namespace test
}  // namespace et

#endif
