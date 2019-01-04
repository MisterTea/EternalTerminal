#ifndef __FAKE_USER_TERMINAL_HPP__
#define __FAKE_USER_TERMINAL_HPP__

#include "Terminal.hpp"

namespace et {
class FakeUserTerminal : public Terminal {
 public:
  virtual pid_t setup(int* fd) {
    // TODO: do we need to create another process?
    // or move the switch case inside PsuedoUserTerminal::runTerminal?
  }

  virtual void runTerminal() {
    // TODO: check masterFd for read and write.
  }

 protected:
  int masterFd;
};
}  // namespace et

#endif