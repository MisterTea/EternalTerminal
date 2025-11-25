#ifndef __CONSOLE_HPP__
#define __CONSOLE_HPP__

#include "ETerminal.pb.h"
#include "Headers.hpp"
#include "RawSocketUtils.hpp"

namespace et {
/**
 * @brief Abstract console interface used by TerminalClient or terminal emulators.
 */
class Console {
 public:
  /** @brief Returns metadata about the console (size, pixels) for the remote client. */
  virtual TerminalInfo getTerminalInfo() = 0;
  /** @brief Prepares the console/terminal before handing control to ET. */
  virtual void setup() = 0;
  /** @brief Restores the console state before exiting ET. */
  virtual void teardown() = 0;
  /** @brief Provides the descriptor that receives terminal output. */
  virtual int getFd() = 0;

  /**
   * @brief Writes UTF-8 to the console using either Windows console APIs or raw fd.
   */
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
