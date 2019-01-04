#ifndef __FAKE_CONSOLE_HPP__
#define __FAKE_CONSOLE_HPP__

#include "Console.hpp"

#include "ETerminal.pb.h"

namespace et {
class FakeConsole : public Console {
 public:
  FakeConsole() : getTerminalInfoCount(0) {}

  virtual ~FakeConsole() {}

  virtual void setup() {
    fakeTerminalInfo.set_row(1);
    fakeTerminalInfo.set_column(1);
    fakeTerminalInfo.set_width(8);
    fakeTerminalInfo.set_height(10);
    // TODO: Create buffer
  }

  virtual void teardown() {
    // TODO: Delete buffer
  }

  virtual TerminalInfo getTerminalInfo() {
    getTerminalInfoCount++;
    if (getTerminalInfoCount % 100 == 0) {
      // Bump the terminal info
      fakeTerminalInfo.set_row(fakeTerminalInfo.row() + 1);
    }
    return fakeTerminalInfo;
  }

  virtual int getFd() {
    // TODO: I think we need to use fmemopen()?  We need a char buffer that we
    // can read/write to/from to fake the actual terminal
    throw "Oops";
  }

 protected:
  TerminalInfo fakeTerminalInfo;
  int getTerminalInfoCount;

};  // namespace et
}  // namespace et

#endif