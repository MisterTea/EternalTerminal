#ifndef __BINARY_STDIO_CONSOLE_HPP__
#define __BINARY_STDIO_CONSOLE_HPP__

#ifndef WIN32
#include <fcntl.h>
#include <unistd.h>
#else
#include <io.h>
#include <stdio.h>
#endif

#include "Console.hpp"
#include "ETerminal.pb.h"
#include "RawSocketUtils.hpp"

namespace et {
/**
 * @brief Local stdio console for raw pipe sessions (`et -T`).
 *
 * Does not put the tty in raw mode (binary-safe) and does not advertise window
 * size. Reads from stdin and writes stdout; stderr is handled separately by
 * TerminalClient when TerminalBuffer.is_stderr is set.
 */
class BinaryStdioConsole : public Console {
 public:
  BinaryStdioConsole() = default;
  virtual ~BinaryStdioConsole() {}

  virtual void setup() {
#ifndef WIN32
    int inFlags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (inFlags >= 0) {
      fcntl(STDIN_FILENO, F_SETFL, inFlags | O_NONBLOCK);
    }
    int outFlags = fcntl(STDOUT_FILENO, F_GETFL, 0);
    if (outFlags >= 0) {
      fcntl(STDOUT_FILENO, F_SETFL, outFlags | O_NONBLOCK);
    }
#endif
  }

  virtual void teardown() {}

  virtual std::optional<TerminalInfo> getTerminalInfo() { return std::nullopt; }

  virtual int getFd() {
#ifdef WIN32
    return _fileno(stdout);
#else
    return STDOUT_FILENO;
#endif
  }
};
}  // namespace et

#endif
