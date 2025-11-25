#ifndef __ET_TCP_SOCKET_HANDLER__
#define __ET_TCP_SOCKET_HANDLER__

#include "UnixSocketHandler.hpp"

namespace et {
/**
 * @brief Implements IPv4/IPv6 socket operations built on top of UnixSocketHandler.
 */
class TcpSocketHandler : public UnixSocketHandler {
 public:
  TcpSocketHandler();
  virtual ~TcpSocketHandler() {}

  /**
   * @brief Resolves the hostname/port and connects non-blockingly to the server.
   */
  virtual int connect(const SocketEndpoint& endpoint);
  /**
   * @brief Binds and listens on all IP addresses for the given port.
   */
  virtual set<int> listen(const SocketEndpoint& endpoint);
  /**
   * @brief Returns the listening socket fds associated with a port.
   */
  virtual set<int> getEndpointFds(const SocketEndpoint& endpoint);
  /**
   * @brief Stops listening on the requested port and closes all related fds.
   */
  virtual void stopListening(const SocketEndpoint& endpoint);

  protected:
  /** @brief Tracks all listening sockets created per TCP port. */
  map<int, set<int>> portServerSockets;

  /**
   * @brief Performs additional TCP-specific socket configuration (NODELAY/linger).
   */
  virtual void initSocket(int fd);
};
}  // namespace et

#endif  // __ET_TCP_SOCKET_HANDLER__
