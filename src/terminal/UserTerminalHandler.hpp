#ifndef __ET_USER_TERMINAL_HANDLER__
#define __ET_USER_TERMINAL_HANDLER__

#include "Headers.hpp"
#include "SocketHandler.hpp"
#include "UserTerminal.hpp"

namespace et {
/**
 * @brief Manages the lifespan of a `UserTerminal`, feeding data through sockets.
 */
class UserTerminalHandler {
 public:
  /**
   * @brief Initializes the handler with the router endpoint and terminal implementation.
   */
  UserTerminalHandler(shared_ptr<SocketHandler> _socketHandler,
                      shared_ptr<UserTerminal> _term, bool noratelimit,
                      const optional<SocketEndpoint> _routerEndpoint,
                      const string &idPasskey);
  /** @brief Drives the terminal session until cleanup is requested. */
  void run();
  /** @brief Sets a flag to stop the loop and shut down the terminal. */
  void shutdown() {
    lock_guard<recursive_mutex> guard(shutdownMutex);
    shuttingDown = true;
  }

protected:
  /** @brief Router pipe descriptor supplied when the handler was created. */
  int routerFd;
  /** @brief Socket helper used for routing terminal data. */
  shared_ptr<SocketHandler> socketHandler;
  /** @brief Underlying terminal that runs inside the handler. */
  shared_ptr<UserTerminal> term;
  /** @brief Controls whether writes bypass the throttled path. */
  bool noratelimit;
  /** @brief Set by `shutdown()` to stop `run()`. */
  bool shuttingDown;
  /** @brief Guards `shuttingDown` across threads. */
  recursive_mutex shutdownMutex;

  /** @brief Reads from the master fd and forwards data to the client socket. */
  void runUserTerminal(int masterFd);
};
}  // namespace et

#endif  // __ET_ID_PASSKEY_HANDLER__
