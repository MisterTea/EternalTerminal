#ifndef __ET_USER_TERMINAL_ROUTER__
#define __ET_USER_TERMINAL_ROUTER__

#include <optional>

#include "Headers.hpp"
#include "PipeSocketHandler.hpp"
#include "ServerConnection.hpp"

namespace et {

/**
 * @brief Routes authenticated clients to pre-established terminal/user
 * sessions.
 *
 * Exposes a pipe listener for new connections and maps client IDs to
 * `TerminalUserInfo`.
 */
class UserTerminalRouter {
 public:
  /**
   * @brief Builds a router backed by the specified pipe endpoint.
   */
  UserTerminalRouter(shared_ptr<PipeSocketHandler> _socketHandler,
                     const SocketEndpoint& _routerEndpoint);
  /** @brief Returns the active server side descriptor that accepts router
   * clients. */
  inline int getServerFd() { return serverFd; }
  /** @brief Blocks until a new router client connects and returns its id/key
   * info. */
  IdKeyPair acceptNewConnection();

  /**
   * @brief Returns the previously-registered `TerminalUserInfo` for a
   * reconnecting client.
   */
  std::optional<TerminalUserInfo> tryGetInfoForConnection(
      const shared_ptr<ServerClientConnection>& serverClientState);

  /** @brief Returns the router-side socket handler used by this router. */
  inline shared_ptr<PipeSocketHandler> getSocketHandler() {
    return socketHandler;
  }

 protected:
  /** @brief File descriptor used by external clients to reach the router. */
  int serverFd;
  /** @brief Terminal metadata registered by `handleConnection` clients. */
  unordered_map<string, TerminalUserInfo> idInfoMap;
  /** @brief Pipe handler used for communicating with router clients. */
  shared_ptr<PipeSocketHandler> socketHandler;
  /** @brief Synchronizes access to the router state. */
  recursive_mutex routerMutex;
};
}  // namespace et

#endif  // __ET_ID_PASSKEY_ROUTER__
