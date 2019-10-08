#include "Headers.hpp"

#include "ClientConnection.hpp"
#include "SocketHandler.hpp"

namespace et {
class UserJumphostHandler {
 public:
  UserJumphostHandler(shared_ptr<SocketHandler> _jumpClientSocketHandler,
                      const string &_idpasskey,
                      const SocketEndpoint &_dstSocketEndpoint,
                      shared_ptr<SocketHandler> routerSocketHandler,
                      const SocketEndpoint &routerEndpoint);

  void run();
  void shutdown() {
    lock_guard<recursive_mutex> guard(shutdownMutex);
    shuttingDown = true;
  }

 protected:
  shared_ptr<SocketHandler> routerSocketHandler;
  int routerFd;
  shared_ptr<ClientConnection> jumpclient;
  shared_ptr<SocketHandler> jumpClientSocketHandler;
  string idpasskey;
  SocketEndpoint dstSocketEndpoint;
  bool shuttingDown;
  recursive_mutex shutdownMutex;
};
}  // namespace et