#ifndef __ETERNAL_TCP_USER_TERMINAL_ROUTER__
#define __ETERNAL_TCP_USER_TERMINAL_ROUTER__

#include "Headers.hpp"

#include "ServerConnection.hpp"

#define ROUTER_FIFO_NAME "/tmp/etserver.idpasskey.fifo"

namespace et {
class UserTerminalRouter {
 public:
  UserTerminalRouter();
  inline int getServerFd() { return serverFd; }
  void acceptNewConnection(shared_ptr<ServerConnection> globalServer);
  int getFd(const string& id);

 protected:
  int serverFd;
  unordered_map<string, int> idFdMap;
};
}  // namespace et

#endif  // __ETERNAL_TCP_ID_PASSKEY_ROUTER__
