#ifndef __ET_USER_TERMINAL_ROUTER__
#define __ET_USER_TERMINAL_ROUTER__

#include "Headers.hpp"

#include "ServerConnection.hpp"
#include "PipeSocketHandler.hpp"

#define ROUTER_FIFO_NAME "/tmp/etserver.idpasskey.fifo"

namespace et {
class UserTerminalRouter {
 public:
  UserTerminalRouter(const string& routerFifoName);
  inline int getServerFd() { return serverFd; }
  void acceptNewConnection(shared_ptr<ServerConnection> globalServer);
  int getFd(const string& id);

 protected:
  int serverFd;
  unordered_map<string, int> idFdMap;
  PipeSocketHandler socketHandler;
};
}  // namespace et

#endif  // __ET_ID_PASSKEY_ROUTER__
