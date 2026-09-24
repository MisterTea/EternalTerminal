#ifndef __ET_TERMINAL_SERVER__
#define __ET_TERMINAL_SERVER__

#include <chrono>

#include "ClientConnection.hpp"
#include "CryptoHandler.hpp"
#include "DaemonCreator.hpp"
#include "ETerminal.pb.h"
#include "Headers.hpp"
#include "LogHandler.hpp"
#include "PortForwardHandler.hpp"
#include "ServerConnection.hpp"
#include "TcpSocketHandler.hpp"
#include "UserTerminalHandler.hpp"
#include "UserTerminalRouter.hpp"

namespace et {
/**
 * @brief Eternal terminal server that accepts clients and routes them to jump
 * hosts or terminals.
 *
 * Manages a router socket, per-client terminal threads, and forwards new
 * connections to `runTerminal` or `runJumpHost`.
 */
class TerminalServer : public ServerConnection {
 public:
  /** @brief Initializes the server with socket helpers and router endpoint. */
  TerminalServer(std::shared_ptr<SocketHandler> _socketHandler,
                 const SocketEndpoint& _serverEndpoint,
                 std::shared_ptr<PipeSocketHandler> _pipeSocketHandler,
                 const SocketEndpoint& _routerEndpoint);
  /** @brief Tears down the server, closing any active router connections. */
  virtual ~TerminalServer();
  /** @brief Drives a jumphost proxy session for the authenticated client. */
  void runJumpHost(shared_ptr<ServerClientConnection> serverClientState,
                   const InitialPayload& payload,
                   const TerminalUserInfo& userInfo);
  /** @brief Launches the interactive terminal session for a client. */
  void runTerminal(shared_ptr<ServerClientConnection> serverClientState,
                   const InitialPayload& payload,
                   const TerminalUserInfo& userInfo, bool resume,
                   bool* terminalEofOut);
  /** @brief Sets up the client state and pushes it into the terminal router. */
  void handleConnection(shared_ptr<ServerClientConnection> serverClientState);
  void finishSession(
      const shared_ptr<ServerClientConnection>& serverClientState,
      const std::optional<TerminalUserInfo>& userInfo, bool terminalEof);
  // Skips the INITIAL_PAYLOAD bootstrap for a terminal that re-registered.
  void handleConnectionResume(
      shared_ptr<ServerClientConnection> serverClientState);
  /** @brief Callback from ServerConnection when a new client is authenticated.
   */
  virtual bool newClient(shared_ptr<ServerClientConnection> serverClientState);
  virtual bool shouldResumeAsReturning(const string& clientId);
  virtual void resumeClient(shared_ptr<ServerClientConnection> state);

  /** @brief Main loop that accepts client connections and relays to handlers.
   */
  void run();
  /** @brief Signals the server loop to stop accepting new work. */
  void shutdown() {
    lock_guard<std::mutex> guard(terminalThreadMutex);
    halt = true;
  }

  /**
   * @brief How long a terminal may stay disconnected before it is closed.
   *
   * `0` disables the timeout. The `etserver` flag is in minutes; this setter
   * takes seconds so tests can use a short deadline.
   */
  void setDisconnectTimeoutSeconds(int seconds) {
    disconnectTimeoutSec = seconds;
  }

  int getDisconnectTimeoutSeconds() const { return disconnectTimeoutSec; }

  // A re-registered terminal has no pump until its client returns, so run()
  // keeps its disconnect clock instead.
  void trackUnclaimedResume(const string& id,
                            std::chrono::steady_clock::time_point now);
  void expireUnclaimedResumes(std::chrono::steady_clock::time_point now);

  // Call only after run() has exited; it still selects on these fds.
  void shutdownConnections() {
    ServerConnection::shutdown();
    terminalRouter->shutdown();
  }

  /** @brief Router that hands reconnecting clients to their terminals. */
  shared_ptr<UserTerminalRouter> terminalRouter;
  /** @brief Threads that manage active terminal/jumphost sessions. */
  vector<shared_ptr<thread>> terminalThreads;
  /** @brief Flag that stops the accept loop when true. */
  bool halt = false;

 protected:
  /**
   * @brief Seconds to wait for a client's initial payload before giving up.
   * Overridable so tests do not have to wait out the real deadline.
   */
  int initialPayloadTimeoutSec = INITIAL_PAYLOAD_TIMEOUT_DURATION;
  /**
   * @brief Seconds a disconnected etterminal may live. `0` means no timeout.
   */
  int disconnectTimeoutSec = 0;
  // Touched only by the run() thread.
  map<string, std::chrono::steady_clock::time_point> unclaimedResumes;
  /** @brief Guards access to `terminalThreads` and the halt flag. */
  mutex terminalThreadMutex;
  /** @brief Local pipe endpoint used to signal terminal/jumphost handoffs. */
  SocketEndpoint routerEndpoint;
};
/**
 * @brief Monotonic stamp for how long a terminal has been without a client.
 *
 * A non-positive timeout never fires. A connected client clears the stamp.
 * The first disconnected observation records `now` and does not fire; a later
 * observation fires once steady time reaches the timeout. Wall-clock jumps
 * do not move `std::chrono::steady_clock`.
 */
struct DisconnectDeadline {
  std::chrono::steady_clock::time_point since{};
  bool started = false;
};

inline bool disconnectDeadlineReached(DisconnectDeadline* deadline,
                                      std::chrono::steady_clock::time_point now,
                                      bool connected, int timeoutSec) {
  if (deadline == nullptr) {
    return false;
  }
  if (timeoutSec <= 0 || connected) {
    deadline->started = false;
    return false;
  }
  if (!deadline->started) {
    deadline->since = now;
    deadline->started = true;
    return false;
  }
  return now - deadline->since >= std::chrono::seconds(timeoutSec);
}

/**
 * @brief True when a stale disconnect snapshot should close the session.
 * `currentSocketFd > 0` means a reconnect already landed and must win.
 */
inline bool disconnectExpiryClosesSession(
    int observedSocketFd, int currentSocketFd, bool alreadyShuttingDown,
    DisconnectDeadline* deadline, std::chrono::steady_clock::time_point now,
    int timeoutSec) {
  if (alreadyShuttingDown || currentSocketFd > 0) {
    return false;
  }
  const bool stillConnected = observedSocketFd > 0;
  return disconnectDeadlineReached(deadline, now, stillConnected, timeoutSec);
}

}  // namespace et

#endif  // __ET_TERMINAL_SERVER__
