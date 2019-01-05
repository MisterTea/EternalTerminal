#ifndef __ET_USER_TERMINAL_HANDLER__
#define __ET_USER_TERMINAL_HANDLER__

#include "Headers.hpp"

#include "SocketHandler.hpp"
#include "UserTerminal.hpp"

namespace et {
class UserTerminalHandler {
 public:
  UserTerminalHandler(shared_ptr<SocketHandler> _socketHandler,
                      shared_ptr<UserTerminal> _term, bool noratelimit);
  void connectToRouter(const string& idPasskey);
  void run();

 protected:
  int routerFd;
  shared_ptr<SocketHandler> socketHandler;
  shared_ptr<UserTerminal> term;
  bool noratelimit;

  void runUserTerminal(int masterFd);
};
}  // namespace et

#endif  // __ET_ID_PASSKEY_HANDLER__
