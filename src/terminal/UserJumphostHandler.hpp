#include "ClientConnection.hpp"
#include "Headers.hpp"
#include "SocketHandler.hpp"

namespace et {
/**
 * @brief Proxies jumphost traffic between the router pipe and a remote
 * endpoint.
 */
class UserJumphostHandler {
 public:
  /**
   * @brief Bridges a jumphost client over the router pipe to the destination
   * endpoint.
   */
  UserJumphostHandler(shared_ptr<SocketHandler> _jumpClientSocketHandler,
                      const string &_idpasskey,
                      const SocketEndpoint &_dstSocketEndpoint,
                      shared_ptr<SocketHandler> routerSocketHandler,
                      const optional<SocketEndpoint> routerEndpoint);

  /** @brief Runs the jumphost loop until shutdown is requested. */
  void run();
  /** @brief Signals the handler thread to stop processing. */
  void shutdown() {
    lock_guard<recursive_mutex> guard(shutdownMutex);
    shuttingDown = true;
  }

 protected:
  /** @brief Router side handler used to accept jumphost connections. */
  shared_ptr<SocketHandler> routerSocketHandler;
  /** @brief File descriptor for the router pipe endpoint. */
  int routerFd;
  /** @brief Client connection used when forwarding traffic to the destination.
   */
  shared_ptr<ClientConnection> jumpclient;
  /** @brief Optional handler that accepts requests from the jumphost. */
  shared_ptr<SocketHandler> jumpClientSocketHandler;
  /** @brief Passkey/id pair used to authenticate the jumphost. */
  string idpasskey;
  /** @brief Destination socket endpoint that receives forwarded data. */
  SocketEndpoint dstSocketEndpoint;
  /** @brief Signals that the handler should stop accepting work. */
  bool shuttingDown;
  /** @brief Guards reads/writes to the `shuttingDown` flag. */
  recursive_mutex shutdownMutex;
};
}  // namespace et
