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
#include "SystemUtils.hpp"
#include "TcpSocketHandler.hpp"
#include "UserTerminalHandler.hpp"
#include "UserTerminalRouter.hpp"

namespace et {

class TerminalServer : public ServerConnection {
 public:
  TerminalServer(std::shared_ptr<SocketHandler> _socketHandler,
                 const SocketEndpoint &_serverEndpoint,
                 std::shared_ptr<PipeSocketHandler> _pipeSocketHandler,
                 const SocketEndpoint &_routerEndpoint);
  virtual ~TerminalServer();
  void runJumpHost(shared_ptr<ServerClientConnection> serverClientState,
                   const InitialPayload &payload);
  void runTerminal(shared_ptr<ServerClientConnection> serverClientState,
                   const InitialPayload &payload);
  void handleConnection(shared_ptr<ServerClientConnection> serverClientState);
  virtual bool newClient(shared_ptr<ServerClientConnection> serverClientState);

  void run();
  void shutdown() {
    lock_guard<std::mutex> guard(terminalThreadMutex);
    halt = true;
  }

  shared_ptr<UserTerminalRouter> terminalRouter;

  vector<shared_ptr<thread>> terminalThreads;
  bool halt = false;

 protected:
  mutex terminalThreadMutex;
  SocketEndpoint routerEndpoint;
};
}  // namespace et

#endif  // __ET_TERMINAL_SERVER__
