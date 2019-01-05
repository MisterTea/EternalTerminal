#ifndef __USER_TERMINAL_HPP__
#define __USER_TERMINAL_HPP__

#include "Headers.hpp"

#include "ETerminal.pb.h"

namespace et {
class UserTerminal {
 public:
  virtual ~UserTerminal() {}

  virtual int setup(int routerFd) = 0;
  virtual void runTerminal() = 0;
  virtual void handleSessionEnd() = 0;
  virtual void cleanup() = 0;
  virtual void setInfo(const winsize& tmpwin) = 0;
};
}  // namespace et

#endif