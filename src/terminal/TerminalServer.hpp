#ifndef __ET_TERMINAL_SERVER__
#define __ET_TERMINAL_SERVER__

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
                 const SocketEndpoint &_serverEndpoint,
                 std::shared_ptr<PipeSocketHandler> _pipeSocketHandler,
                 const SocketEndpoint &_routerEndpoint);
  /** @brief Tears down the server, closing any active router connections. */
  virtual ~TerminalServer();
  /** @brief Drives a jumphost proxy session for the authenticated client. */
  void runJumpHost(shared_ptr<ServerClientConnection> serverClientState,
                   const InitialPayload &payload);
  /** @brief Launches the interactive terminal session for a client. */
  void runTerminal(shared_ptr<ServerClientConnection> serverClientState,
                   const InitialPayload &payload);
  /** @brief Sets up the client state and pushes it into the terminal router. */
  void handleConnection(shared_ptr<ServerClientConnection> serverClientState);
  /** @brief Callback from ServerConnection when a new client is authenticated.
   */
  virtual bool newClient(shared_ptr<ServerClientConnection> serverClientState);

  /** @brief Main loop that accepts client connections and relays to handlers.
   */
  void run();
  /** @brief Signals the server loop to stop accepting new work. */
  void shutdown() {
    lock_guard<std::mutex> guard(terminalThreadMutex);
    halt = true;
  }

  /** @brief Router that hands reconnecting clients to their terminals. */
  shared_ptr<UserTerminalRouter> terminalRouter;
  /** @brief Threads that manage active terminal/jumphost sessions. */
  vector<shared_ptr<thread>> terminalThreads;
  /** @brief Flag that stops the accept loop when true. */
  bool halt = false;

 protected:
  /** @brief Guards access to `terminalThreads` and the halt flag. */
  mutex terminalThreadMutex;
  /** @brief Local pipe endpoint used to signal terminal/jumphost handoffs. */
  SocketEndpoint routerEndpoint;
};
}  // namespace et

#endif  // __ET_TERMINAL_SERVER__
