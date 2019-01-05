#ifndef __FAKE_USER_TERMINAL_HPP__
#define __FAKE_USER_TERMINAL_HPP__

#include "UserTerminal.hpp"

namespace et {
class FakeUserTerminal : public UserTerminal {
 public:
  virtual ~FakeUserTerminal() : didCleanUp(false), didHandleSessionEnd(false) {
    memset(&lastWinInfo, 0, sizeof(winsize));
  }

  virtual int setup(int routerFd) = 0;
  virtual void runTerminal() = 0;
  virtual void handleSessionEnd() { didHandleSessionEnd = true; }
  virtual void cleanup() { didCleanUp = true; }
  virtual void setInfo(const winsize& tmpwin) { lastWinInfo = tmpwin; }

  bool didCleanUp;
  bool didHandleSessionEnd;
  winsize lastWinInfo;
};
}  // namespace et

#endif