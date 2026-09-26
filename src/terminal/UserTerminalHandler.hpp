#ifndef __ET_USER_TERMINAL_HANDLER__
#define __ET_USER_TERMINAL_HANDLER__

#include "Headers.hpp"
#include "SocketHandler.hpp"
#include "TmuxCcFilter.hpp"
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
  /** @brief True when TermInit requested a raw pipe command session. */
  bool pipeMode;
  /**
   * @brief Removes journald/wall lines from a tmux -CC byte stream.
   * Shell output before control mode is left alone.
   */
  TmuxCcInjectionFilter controlOutputFilter_;
  string id;
  string passkey;
  optional<SocketEndpoint> routerEndpoint;
  bool ptyActive;
  bool hadReverseTunnels;
  // From TermInit; re-advertised on router re-register after etserver restart.
  std::optional<int32_t> disconnectTimeoutSeconds;

  /** @brief Reads from the master fd and forwards data to the client socket. */
  void runUserTerminal(int masterFd);
  /** @brief Holds the router open without a pty or a shell (ssh -W). */
  void runIdleSession();
  /** @brief Forwards terminal output to the router as TERMINAL_BUFFER. */
  void forwardOutputToRouter(const char* data, size_t length, bool isStderr);
  /** @brief Reaps the child and sends TERMINAL_EXIT_STATUS to the router. */
  void finishSession();
#ifdef WIN32
  /** @brief Pumps a ConPTY terminal (see PseudoUserTerminal). */
  void runConPtyTerminal(class PseudoUserTerminal& conpty);
  /** @brief Pumps a socket-backed terminal (test doubles, same protocol). */
  void runSocketTerminal(int masterFd);
#endif

  void registerWithRouter();

  // Retries with backoff until the router is back; returns -1 on shutdown.
  // The master fd is not drained meanwhile, so the shell blocks on output.
  int reconnectRouter();
};
}  // namespace et

#endif  // __ET_ID_PASSKEY_HANDLER__
