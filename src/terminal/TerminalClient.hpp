#ifndef __ET_TERMINAL_CLIENT__
#define __ET_TERMINAL_CLIENT__

#include "ClientConnection.hpp"
#include "Console.hpp"
#include "CryptoHandler.hpp"
#include "ETerminal.pb.h"
#include "ForwardSourceHandler.hpp"
#include "Headers.hpp"
#include "LogHandler.hpp"
#include "PortForwardHandler.hpp"
#include "RawSocketUtils.hpp"
#include "ServerConnection.hpp"
#include "SshSetupHandler.hpp"
#include "TcpSocketHandler.hpp"

namespace et {
/**
 * @brief Coordinates the lifecycle of a client connection, console, and
 * tunnels.
 */
class TerminalClient {
 public:
  /**
   * @brief Message thrown when a reattach fails because the server has no
   * session for the saved id anymore (ConnectStatus INVALID_KEY).
   */
  static const string INVALID_SESSION_CONNECT_ERROR;

  /**
   * @brief Configures the client with the required sockets, console, and
   * tunnels.
   * @param _maxConnectAttempts Initial connect attempts before giving up.
   * @param _exitOnConnectFailure When false, a failed initial connect throws
   * instead of exiting so callers can clean up (used by --attach).
   */
  TerminalClient(std::shared_ptr<SocketHandler> _socketHandler,
                 std::shared_ptr<SocketHandler> _pipeSocketHandler,
                 const SocketEndpoint& _socketEndpoint, const string& id,
                 const string& passkey, shared_ptr<Console> _console,
                 bool jumphost, const string& tunnels,
                 const string& reverseTunnels, bool forwardSshAgent,
                 const string& identityAgent, int _keepaliveDuration,
                 const vector<pair<string, string>>& envVars,
                 int _maxConnectAttempts = 3,
                 bool _exitOnConnectFailure = true);
  /** @brief Tears down the client, closing sockets and stopping background
   * threads. */
  virtual ~TerminalClient();
  /** @brief Runs the interactive session for `command`, optionally staying
   * alive. */
  void run(const string& command, const bool noexit);
  /**
   * @brief Flags the client loop to exit gracefully on the next iteration.
   */
  void shutdown() {
    lock_guard<recursive_mutex> guard(shutdownMutex);
    shuttingDown = true;
  }

 protected:
  /** @brief Console wrapper used for local terminal input/output. */
  shared_ptr<Console> console;
  /** @brief Client connection that talks to the ET server. */
  shared_ptr<ClientConnection> connection;
  /** @brief Handles local/remote port forwarding tunnels. */
  shared_ptr<PortForwardHandler> portForwardHandler;
  /** @brief Guarded flag that ends `run()` when set. */
  bool shuttingDown;
  /** @brief Synchronizes writes to `shuttingDown`. */
  recursive_mutex shutdownMutex;
  /** @brief Keepalive interval (seconds) sent to the server. */
  int keepaliveDuration;
};

}  // namespace et
#endif  // __ET_TERMINAL_CLIENT__
