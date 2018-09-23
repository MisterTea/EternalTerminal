#ifndef __ET_USER_TERMINAL_HANDLER__
#define __ET_USER_TERMINAL_HANDLER__

#include "Headers.hpp"

#include "PipeSocketHandler.hpp"

namespace et {
class UserTerminalHandler {
 public:
  UserTerminalHandler();
  void connectToRouter(const string& idPasskey);
  void run();

 protected:
  int routerFd;
  PipeSocketHandler socketHandler;

  void runUserTerminal(int masterFd, pid_t childPid);
};
}  // namespace et

#endif  // __ET_ID_PASSKEY_HANDLER__
