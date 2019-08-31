#include "Headers.hpp"

#include "ClientConnection.hpp"
#include "SocketHandler.hpp"

namespace et {
class UserJumphostHandler {
 public:
  UserJumphostHandler(shared_ptr<SocketHandler> jumpClientSocketHandlerHandler,
                      const string &idpasskey,
                      const SocketEndpoint &dstSocketEndpoint,
                      shared_ptr<SocketHandler> routerSocketHandler,
                      const SocketEndpoint &routerEndpoint);

  void run();

 protected:
  shared_ptr<SocketHandler> routerSocketHandler;
  int routerFd;
  shared_ptr<ClientConnection> jumpclient;
};
}  // namespace et