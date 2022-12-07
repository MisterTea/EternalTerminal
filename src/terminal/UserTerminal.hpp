#ifndef __USER_TERMINAL_HPP__
#define __USER_TERMINAL_HPP__

#include "ETerminal.pb.h"
#include "Headers.hpp"

namespace et {
class UserTerminal {
 public:
  virtual ~UserTerminal() {}

  virtual int setup(int routerFd) = 0;
  virtual void runTerminal() = 0;
  virtual void handleSessionEnd() = 0;
  virtual void cleanup() = 0;
  virtual int getFd() = 0;
  virtual void setInfo(const winsize& tmpwin) = 0;
};
}  // namespace et

#endif