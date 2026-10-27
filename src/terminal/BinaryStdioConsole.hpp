#ifndef __BINARY_STDIO_CONSOLE_HPP__
#define __BINARY_STDIO_CONSOLE_HPP__

#include "StdioConsole.hpp"

namespace et {
/**
 * @brief Local stdio console for raw pipe sessions (`et -T`).
 *
 * Does not put the tty in raw mode (binary-safe) and does not advertise window
 * size. Reads from stdin and writes stdout; stderr is handled separately by
 * TerminalClient when TerminalBuffer.is_stderr is set.
 */
class BinaryStdioConsole : public StdioConsole {
 public:
  void teardown() override {}

  std::optional<TerminalInfo> getTerminalInfo() override {
    return std::nullopt;
  }
};
}  // namespace et

#endif
