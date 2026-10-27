#include <iostream>

#include "PseudoTerminalConsole.hpp"

namespace et {
struct PseudoTerminalConsole::SavedState {
  DWORD inputMode = 0;
  DWORD outputMode = 0;
};

PseudoTerminalConsole::PseudoTerminalConsole() : saved(new SavedState()) {
  GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &saved->inputMode);
  GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &saved->outputMode);
}

PseudoTerminalConsole::~PseudoTerminalConsole() = default;

void PseudoTerminalConsole::setup() {
  // Disable processed input so Ctrl-C is delivered as terminal input instead
  // of terminating the local client. This matches the Unix raw-mode path.
  const DWORD rawInputMode =
      (saved->inputMode &
       ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) |
      ENABLE_VIRTUAL_TERMINAL_INPUT;
  SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), rawInputMode);
  SetConsoleMode(
      GetStdHandle(STD_OUTPUT_HANDLE),
      saved->outputMode | ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT |
          ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
  // DISABLE_NEWLINE_AUTO_RETURN is needed to keep full-screen terminal apps
  // like tmux from scrolling incorrectly, but some Windows terminal hosts can
  // leave long interactive input repainting over one visual row after this
  // mode change. Reassert DECAWM so readline-style input wraps normally.
  std::cout << "\033[?7h" << std::flush;
  StdioConsole::setup();
}

void PseudoTerminalConsole::teardown() {
  SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), saved->inputMode);
  SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), saved->outputMode);
}

std::optional<TerminalInfo> PseudoTerminalConsole::getTerminalInfo() {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
    return std::nullopt;
  }
  TerminalInfo ti;
  ti.set_column(csbi.srWindow.Right - csbi.srWindow.Left + 1);
  ti.set_row(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
  return ti;
}
}  // namespace et
