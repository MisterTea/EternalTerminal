#ifndef __CONSOLE_HPP__
#define __CONSOLE_HPP__

#include "Headers.hpp"

#include "RawSocketUtils.hpp"

namespace et {
class Console {
 public:
  virtual TerminalInfo getTerminalInfo() = 0;
  virtual void setup() = 0;
  virtual void teardown() = 0;
  virtual int getFd() = 0;

  virtual void write(const string& s) {
    RawSocketUtils::writeAll(getFd(), &s[0], s.length());
  }
};
}  // namespace et

#endif