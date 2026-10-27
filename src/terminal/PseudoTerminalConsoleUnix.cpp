#include "PseudoTerminalConsole.hpp"

namespace et {
struct PseudoTerminalConsole::SavedState {
  termios terminal;
};

PseudoTerminalConsole::PseudoTerminalConsole() : saved(new SavedState()) {
  tcgetattr(STDIN_FILENO, &saved->terminal);
}

PseudoTerminalConsole::~PseudoTerminalConsole() = default;

void PseudoTerminalConsole::setup() {
  termios terminal_local;
  tcgetattr(STDIN_FILENO, &terminal_local);
  saved->terminal = terminal_local;
  cfmakeraw(&terminal_local);
  tcsetattr(STDIN_FILENO, TCSANOW, &terminal_local);
  StdioConsole::setup();
}

void PseudoTerminalConsole::teardown() {
  tcsetattr(STDIN_FILENO, TCSANOW, &saved->terminal);
}

std::optional<TerminalInfo> PseudoTerminalConsole::getTerminalInfo() {
  winsize win{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &win) < 0) {
    return std::nullopt;
  }
  TerminalInfo ti;
  ti.set_row(win.ws_row);
  ti.set_column(win.ws_col);
  ti.set_width(win.ws_xpixel);
  ti.set_height(win.ws_ypixel);
  return ti;
}
}  // namespace et
