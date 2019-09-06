#ifndef __ET_TERMINAL_SERVER__
#define __ET_TERMINAL_SERVER__

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if __APPLE__
#include <util.h>
#elif __FreeBSD__
#include <libutil.h>
#include <sys/socket.h>
#elif __NetBSD__  // do not need pty.h on NetBSD
#else
#include <pty.h>
#include <signal.h>
#endif

#include "ETerminal.pb.h"

#include "ClientConnection.hpp"
#include "CryptoHandler.hpp"
#include "DaemonCreator.hpp"
#include "Headers.hpp"
#include "LogHandler.hpp"
#include "PortForwardHandler.hpp"
#include "ServerConnection.hpp"
#include "SystemUtils.hpp"
#include "TcpSocketHandler.hpp"
#include "UserTerminalHandler.hpp"
#include "UserTerminalRouter.hpp"

#include "simpleini/SimpleIni.h"

namespace et {

class TerminalServer : public ServerConnection {
 public:
  TerminalServer(std::shared_ptr<SocketHandler> _socketHandler,
                 const SocketEndpoint &_serverEndpoint,
                 std::shared_ptr<PipeSocketHandler> _pipeSocketHandler,
                 const SocketEndpoint &_routerEndpoint);
  virtual ~TerminalServer();
  void runJumpHost(shared_ptr<ServerClientConnection> serverClientState);
  void runTerminal(shared_ptr<ServerClientConnection> serverClientState,
    const InitialPayload& payload);
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
