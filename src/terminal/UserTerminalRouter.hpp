#ifndef __ET_USER_TERMINAL_ROUTER__
#define __ET_USER_TERMINAL_ROUTER__

#include <optional>

#include "Headers.hpp"
#include "PipeSocketHandler.hpp"
#include "ServerConnection.hpp"

namespace et {
const string ROUTER_FIFO_NAME = GetTempDirectory() + "etserver.idpasskey.fifo";

class UserTerminalRouter {
 public:
  UserTerminalRouter(shared_ptr<PipeSocketHandler> _socketHandler,
                     const SocketEndpoint& _routerEndpoint);
  inline int getServerFd() { return serverFd; }
  IdKeyPair acceptNewConnection();

  std::optional<TerminalUserInfo> tryGetInfoForConnection(
      const shared_ptr<ServerClientConnection>& serverClientState);

  inline shared_ptr<PipeSocketHandler> getSocketHandler() {
    return socketHandler;
  }

 protected:
  int serverFd;
  unordered_map<string, TerminalUserInfo> idInfoMap;
  shared_ptr<PipeSocketHandler> socketHandler;
  recursive_mutex routerMutex;
};
}  // namespace et

#endif  // __ET_ID_PASSKEY_ROUTER__
