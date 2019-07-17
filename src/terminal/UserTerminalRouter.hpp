#ifndef __ET_USER_TERMINAL_ROUTER__
#define __ET_USER_TERMINAL_ROUTER__

#include "Headers.hpp"

#include "PipeSocketHandler.hpp"
#include "ServerConnection.hpp"

#define ROUTER_FIFO_NAME "/tmp/etserver.idpasskey.fifo"

namespace et {
class UserTerminalRouter {
 public:
  UserTerminalRouter(shared_ptr<PipeSocketHandler> _socketHandler,
                     const string& routerFifoName);
  inline int getServerFd() { return serverFd; }
  optional<IdKeyPair> acceptNewConnection();
  int getFd(const string& id);
  inline shared_ptr<PipeSocketHandler> getSocketHandler() {
    return socketHandler;
  }

 protected:
  int serverFd;
  unordered_map<string, int> idFdMap;
  shared_ptr<PipeSocketHandler> socketHandler;
};
}  // namespace et

#endif  // __ET_ID_PASSKEY_ROUTER__
