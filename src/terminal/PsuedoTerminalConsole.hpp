#ifndef __PSUEDO_TERMINAL_CONSOLE_HPP__
#define __PSUEDO_TERMINAL_CONSOLE_HPP__

#include "Console.hpp"
#include "ETerminal.pb.h"
#include "RawSocketUtils.hpp"

namespace et {
class PsuedoTerminalConsole : public Console {
 public:
  PsuedoTerminalConsole() {
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

  virtual ~PsuedoTerminalConsole() {}

  virtual void setup() {
#ifdef WIN32
    auto hstdin = GetStdHandle(STD_INPUT_HANDLE);
    SetConsoleMode(hstdin, ENABLE_VIRTUAL_TERMINAL_INPUT);
    //auto hstdout = GetStdHandle(STD_OUTPUT_HANDLE);
    //SetConsoleMode(hstdout, 0/*ENABLE_VIRTUAL_TERMINAL_PROCESSING*/);
#else
    termios terminal_local;
    tcgetattr(0, &terminal_local);
    memcpy(&terminal_backup, &terminal_local, sizeof(struct termios));
    cfmakeraw(&terminal_local);
    tcsetattr(0, TCSANOW, &terminal_local);
#endif
  }

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

  virtual int getFd() {
#ifdef WIN32
    return _fileno(stdout);
#else
    return STDOUT_FILENO;
#endif
  }

 protected:
#ifdef WIN32
  DWORD inputMode;
  DWORD outputMode;
#else
  termios terminal_backup;
#endif

};  // namespace et
}  // namespace et

#endif
