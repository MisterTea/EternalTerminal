#ifndef __ET_TERMINAL_CLIENT__
#define __ET_TERMINAL_CLIENT__

#include "ClientConnection.hpp"
#include "Console.hpp"
#include "CryptoHandler.hpp"
#include "Headers.hpp"
#include "LogHandler.hpp"
#include "PortForwardHandler.hpp"
#include "PortForwardSourceHandler.hpp"
#include "RawSocketUtils.hpp"
#include "ServerConnection.hpp"
#include "SshSetupHandler.hpp"
#include "TcpSocketHandler.hpp"

#include <errno.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>

#include "ETerminal.pb.h"

namespace et {
class TerminalClient {
 public:
  TerminalClient(std::shared_ptr<SocketHandler> _socketHandler,
                 const SocketEndpoint& _socketEndpoint, const string& id,
                 const string& passkey, shared_ptr<Console> _console);
  virtual ~TerminalClient();
  void setUpTunnel(const string& tunnels);
  void setUpReverseTunnels(const string& reverseTunnels);
  void handleWindowChanged(winsize* win);
  // void handlePfwPacket(char packetType);
  void run(const string& command, const string& tunnels,
           const string& reverseTunnels);
  void shutdown() {
    shuttingDown = true;
  }

 protected:
  shared_ptr<Console> console;
  shared_ptr<ClientConnection> connection;
  shared_ptr<PortForwardHandler> portForwardHandler;
  bool shuttingDown;
};

}  // namespace et
#endif  // __ET_TERMINAL_CLIENT__
