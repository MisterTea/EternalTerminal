#ifndef __CONSOLE_HPP__
#define __CONSOLE_HPP__

#include <optional>

#include "ETerminal.pb.h"
#include "Headers.hpp"
#include "RawSocketUtils.hpp"

namespace et {
/**
 * @brief Abstract console interface used by TerminalClient or terminal
 * emulators.
 */
class Console {
 public:
  /** @brief Returns console dimensions, or no value when they cannot be read.
   */
  virtual std::optional<TerminalInfo> getTerminalInfo() = 0;
  /** @brief Prepares the console/terminal before handing control to ET. */
  virtual void setup() = 0;
  /** @brief Restores the console state before exiting ET. */
  virtual void teardown() = 0;
  /** @brief Provides the descriptor that receives terminal output. */
  virtual int getFd() = 0;

  /**
   * @brief Writes UTF-8 to the console using either Windows console APIs or raw
   * fd.
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

  /**
   * @brief Write as many bytes as the console will accept without blocking.
   * @return Bytes written. 0 means try again when the fd is writable.
   */
  virtual size_t writeSome(const string& s) {
    if (s.empty()) {
      return 0;
    }
#ifdef WIN32
    write(s);
    return s.size();
#else
    ssize_t rc = ::write(getFd(), s.data(), s.size());
    if (rc < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return 0;
      }
      throw std::runtime_error(string("console write failed: ") +
                               strerror(errno));
    }
    return static_cast<size_t>(rc);
#endif
  }
};
}  // namespace et

#endif
