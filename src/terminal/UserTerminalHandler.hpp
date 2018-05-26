#ifndef __ETERNAL_TCP_USER_TERMINAL_HANDLER__
#define __ETERNAL_TCP_USER_TERMINAL_HANDLER__

#include "Headers.hpp"

namespace et {
class UserTerminalHandler {
 public:
  UserTerminalHandler();
  void connectToRouter(const string& idPasskey);
  void run();

 protected:
  int routerFd;

  void runUserTerminal(int masterFd, pid_t childPid);
};
}  // namespace et

#endif  // __ETERNAL_TCP_ID_PASSKEY_HANDLER__
