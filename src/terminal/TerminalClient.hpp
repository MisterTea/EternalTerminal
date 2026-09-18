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
   * @brief Configures the client with the required sockets, console, and
   * tunnels.
   */
  TerminalClient(std::shared_ptr<SocketHandler> _socketHandler,
                 std::shared_ptr<SocketHandler> _pipeSocketHandler,
                 const SocketEndpoint& _socketEndpoint, const string& id,
                 const string& passkey, shared_ptr<Console> _console,
                 bool jumphost, const string& tunnels,
                 const string& reverseTunnels, bool forwardSshAgent,
                 const string& identityAgent, int _keepaliveDuration,
                 const vector<pair<string, string>>& envVars,
                 bool attachExisting = false,
                 std::function<pair<string, string>()> bootstrapNewSession =
                     nullptr);
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

  // True when the client currently holds a live connection to etserver.  ET
  // flips this to false during a drop and back to true once it reconnects, so
  // it distinguishes "link down" from "the daemon is gone" (the latter shows as
  // an unreachable control socket).
  bool isConnected() {
    return connection && !connection->isDisconnected();
  }

  // True when this client adopted a session that was already running rather
  // than creating one. The shell is mid-life, so connect-time setup has already
  // happened and re-running it would type into whatever is in the foreground.
  bool attachedToExisting() { return attachedExisting; }

  /**
   * @brief Why `run()` returned, in a form fit to show a user.
   *
   * `run()` returning is what ends a control session, and every caller so far
   * has had to guess which of several very different things happened. Ask here
   * instead of inferring it.
   */
  string exitReason() {
    if (connection && connection->serverEndedSession()) {
      return "the remote session ended (server no longer has it)";
    }
    {
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        return "shutdown was requested";
      }
    }
    return "the connection closed";
  }

  // True when the server refused a reconnect because it no longer holds this
  // session, as opposed to any other reason the client stopped.
  bool sessionEndedByServer() {
    return connection && connection->serverEndedSession();
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
  /** @brief Set when the constructor adopted an already-running session. */
  bool attachedExisting = false;
};

}  // namespace et
#endif  // __ET_TERMINAL_CLIENT__
