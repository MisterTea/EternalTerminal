#ifndef __PSUEDO_TERMINAL_CONSOLE_HPP__
#define __PSUEDO_TERMINAL_CONSOLE_HPP__

#include "Console.hpp"
#include "ETerminal.pb.h"
#include "RawSocketUtils.hpp"

#ifdef WIN32
#include <iostream>
#endif

namespace et {
/**
 * @brief Configures the local console into raw mode and exposes terminal info.
 */
class PseudoTerminalConsole : public Console {
 public:
  PseudoTerminalConsole() {
#ifdef WIN32
    auto hstdin = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(hstdin, &inputMode);
    auto hstdout = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleMode(hstdout, &outputMode);
#else
    termios terminal_local;
    tcgetattr(0, &terminal_local);
    memcpy(&terminal_backup, &terminal_local, sizeof(struct termios));
#endif
  }

  virtual ~PseudoTerminalConsole() {}

  /** @brief Switches stdin/out to raw mode for terminal I/O. */
  virtual void setup() {
#ifdef WIN32
    auto hstdin = GetStdHandle(STD_INPUT_HANDLE);
    // Disable processed input so Ctrl-C is delivered as terminal input instead
    // of terminating the local client. This matches the Unix raw-mode path.
    DWORD rawInputMode =
        (inputMode &
         ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) |
        ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(hstdin, rawInputMode);
    auto hstdout = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleMode(hstdout, outputMode | ENABLE_PROCESSED_OUTPUT |
                                ENABLE_WRAP_AT_EOL_OUTPUT |
                                ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                DISABLE_NEWLINE_AUTO_RETURN);
    // DISABLE_NEWLINE_AUTO_RETURN is needed to keep full-screen terminal apps
    // like tmux from scrolling incorrectly, but some Windows terminal hosts can
    // leave long interactive input repainting over one visual row after this
    // mode change. Reassert DECAWM so readline-style input wraps normally.
    std::cout << "\033[?7h" << std::flush;
#else
    termios terminal_local;
    tcgetattr(0, &terminal_local);
    memcpy(&terminal_backup, &terminal_local, sizeof(struct termios));
    cfmakeraw(&terminal_local);
    tcsetattr(0, TCSANOW, &terminal_local);
#endif
  }

  /** @brief Restores the terminal state saved during construction. */
  virtual void teardown() {
#ifdef WIN32
    auto hstdin = GetStdHandle(STD_INPUT_HANDLE);
    SetConsoleMode(hstdin, inputMode);
    auto hstdout = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleMode(hstdout, outputMode);
#else
    tcsetattr(0, TCSANOW, &terminal_backup);
#endif
  }

  /** @brief Queries the current terminal window dimensions. */
  virtual TerminalInfo getTerminalInfo() {
#ifdef WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    int columns, rows;

    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
      STFATAL << "Error getting console info: " << GetLastError();
    }
    TerminalInfo ti;
    ti.set_column(csbi.srWindow.Right - csbi.srWindow.Left + 1);
    ti.set_row(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);

    /* TODO: Find out why this does not work
    HWND myconsole = GetConsoleWindow();
    HDC mydc = GetDC(myconsole);
    RECT rect;
    GetClientRect(myconsole, &rect);
    ti.set_height(rect.bottom - rect.top);
    ti.set_width(rect.right - rect.left);
    */

    return ti;
#else
    winsize win;
    ioctl(1, TIOCGWINSZ, &win);
    TerminalInfo ti;
    ti.set_row(win.ws_row);
    ti.set_column(win.ws_col);
    ti.set_width(win.ws_xpixel);
    ti.set_height(win.ws_ypixel);
    return ti;
#endif
  }

  /** @brief Returns the file descriptor linked to stdout. */
  virtual int getFd() {
#ifdef WIN32
    return _fileno(stdout);
#else
    return STDOUT_FILENO;
#endif
  }

 protected:
#ifdef WIN32
  /** @brief Saved console input mode so `teardown()` can restore it. */
  DWORD inputMode;
  /** @brief Saved console output mode for restoration. */
  DWORD outputMode;
#else
  /** @brief Backup of the terminal's `termios` state for teardown. */
  termios terminal_backup;
#endif

};  // namespace et
}  // namespace et

#endif
