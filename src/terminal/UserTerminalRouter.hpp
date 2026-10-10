#ifndef __ET_USER_TERMINAL_ROUTER__
#define __ET_USER_TERMINAL_ROUTER__

#include <chrono>
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
  /** @brief Advances bounded, nonblocking registration reads. Call each tick;
   * returns one completed registration or an empty pair. */
  IdKeyPair acceptNewConnection();
  /** @brief Returns the pending read set after a change, updating generation.
   * Refresh these descriptors in the poller even if numbers were reused. */
  std::optional<set<int>> pendingRegistrationFds(uint64_t& generation) const;

  /**
   * @brief Returns the previously-registered `TerminalUserInfo` for a
   * reconnecting client.
   */
  std::optional<TerminalUserInfo> tryGetInfoForConnection(
      const shared_ptr<ServerClientConnection>& serverClientState);

  void removeConnection(const TerminalUserInfo& userInfo);

  /** @brief Returns the router-side socket handler used by this router. */
  inline shared_ptr<PipeSocketHandler> getSocketHandler() {
    return socketHandler;
  }

  bool isPtyActive(const string& id);

  bool isCurrentRegistration(const string& id, int terminalFd) const;

  // No-op returning false when a newer registration replaced terminalFd.
  bool removeTerminal(const string& id, int terminalFd);

  // Sends TERMINAL_CLOSE, then drops the registration.
  bool closeTerminal(const string& id);

  // Terminals see EOF and keep their pty for a replacement router.
  void shutdown();

 protected:
  struct PendingRegistration {
    string bytes;
    int64_t length = -1;
    std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
  };
  static constexpr size_t MAX_PENDING_REGISTRATIONS = 64;
  static constexpr int64_t MAX_REGISTRATION_LENGTH = 4096;
  static constexpr std::chrono::seconds REGISTRATION_TIMEOUT{30};
  unordered_map<int, PendingRegistration> pendingRegistrations;
  uint64_t pendingFdsGeneration = 0;
  IdKeyPair registerTerminal(int terminalFd, const Packet& packet);
  /** @brief File descriptor used by external clients to reach the router. */
  int serverFd;
  /** @brief Terminal metadata registered by `handleConnection` clients. */
  unordered_map<string, TerminalUserInfo> idInfoMap;
  /** @brief Pipe handler used for communicating with router clients. */
  shared_ptr<PipeSocketHandler> socketHandler;
  /** @brief Synchronizes access to the router state. */
  mutable recursive_mutex routerMutex;
};
}  // namespace et

#endif  // __ET_ID_PASSKEY_ROUTER__
