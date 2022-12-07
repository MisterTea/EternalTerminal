#ifndef __CONSOLE_HPP__
#define __CONSOLE_HPP__

#include "ETerminal.pb.h"
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
#ifdef WIN32
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    std::wstring wide = converter.from_bytes(s);

    auto hstdout = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD numWritten;
    WriteConsole(hstdout, wide.c_str(), wide.length(), &numWritten, NULL);
#else
    RawSocketUtils::writeAll(getFd(), &s[0], s.length());
#endif
  }
};
}  // namespace et

#endif