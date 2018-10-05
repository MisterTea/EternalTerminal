#ifndef __ET_USER_TERMINAL_HANDLER__
#define __ET_USER_TERMINAL_HANDLER__

#include "Headers.hpp"

#include "SocketHandler.hpp"

namespace et {
class UserTerminalHandler {
 public:
  UserTerminalHandler(shared_ptr<SocketHandler> _socketHandler,
                      bool noratelimit);
  void connectToRouter(const string& idPasskey);
  void run();

 protected:
  int routerFd;
  shared_ptr<SocketHandler> socketHandler;
  bool noratelimit;

  void runUserTerminal(int masterFd, pid_t childPid);
};
}  // namespace et

#endif  // __ET_ID_PASSKEY_HANDLER__
