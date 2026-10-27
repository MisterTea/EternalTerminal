#ifndef __PSUEDO_TERMINAL_CONSOLE_HPP__
#define __PSUEDO_TERMINAL_CONSOLE_HPP__

#include "StdioConsole.hpp"

namespace et {
/**
 * @brief Configures the local console into raw mode and exposes terminal info.
 *
 * Platform code lives in PseudoTerminalConsoleUnix.cpp (termios) and
 * PseudoTerminalConsoleWindows.cpp (console modes).
 */
class PseudoTerminalConsole : public StdioConsole {
 public:
  /** @brief Saves the current terminal state for teardown(). */
  PseudoTerminalConsole();
  ~PseudoTerminalConsole() override;

  /** @brief Switches stdin/out to raw mode for terminal I/O. */
  void setup() override;
  /** @brief Restores the saved terminal state. */
  void teardown() override;
  /** @brief Queries the current terminal window dimensions. */
  std::optional<TerminalInfo> getTerminalInfo() override;

 private:
  struct SavedState;
  unique_ptr<SavedState> saved;
};
}  // namespace et

#endif
