#ifndef __TERMINAL_HPP__
#define __TERMINAL_HPP__

#include "Headers.hpp"

#include "ETerminal.pb.h"

namespace et {
class Terminal {
 public:
  virtual pid_t setup(int* fd) = 0;
  virtual void runTerminal() = 0;
};
}  // namespace et

#endif