#ifndef __ET_USER_TERMINAL_HANDLER__
#define __ET_USER_TERMINAL_HANDLER__

#include "Headers.hpp"
#include "SocketHandler.hpp"
#include "UserTerminal.hpp"

namespace et {
/**
 * @brief Manages the lifespan of a `UserTerminal`, feeding data through
 * sockets.
 */
class UserTerminalHandler {
 public:
  /**
   * @brief Initializes the handler with the router endpoint and terminal
   * implementation.
   */
  UserTerminalHandler(shared_ptr<SocketHandler> _socketHandler,
                      shared_ptr<UserTerminal> _term, bool noratelimit,
                      const optional<SocketEndpoint> _routerEndpoint,
                      const string& idPasskey);
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
  /** @brief Client id used for router registration. */
  string id;
  /** @brief Passkey used for router registration. */
  string passkey;
  /** @brief Router endpoint used for (re)connection attempts. */
  optional<SocketEndpoint> routerEndpoint;
  /** @brief True once the pty has been set up (the session has started). */
  bool ptyActive;

  /** @brief Reads from the master fd and forwards data to the client socket. */
  void runUserTerminal(int masterFd);

  /**
   * @brief Connects to the router and sends the TERMINAL_USER_INFO
   * registration for this session.
   * @throws runtime_error when the router cannot be reached.
   */
  void registerWithRouter();

  /**
   * @brief Blocks with bounded backoff until the router is reachable again,
   * re-registering this session with the same id/passkey. The pty child keeps
   * running the whole time; because the master fd is not drained, the shell
   * is backpressured by the kernel pty buffer instead of data being lost.
   * @returns The new router fd, or -1 when the session must end (shutdown).
   */
  int reconnectRouter();
};
}  // namespace et

#endif  // __ET_ID_PASSKEY_HANDLER__
