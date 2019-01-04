#ifndef __PSUEDO_TERMINAL_CONSOLE_HPP__
#define __PSUEDO_TERMINAL_CONSOLE_HPP__

#include "Console.hpp"

#include "ETerminal.pb.h"
#include "RawSocketUtils.hpp"

namespace et {
class PsuedoTerminalConsole : public Console {
 public:
  PsuedoTerminalConsole() {}

  virtual ~PsuedoTerminalConsole() {}

  virtual void setup() {
    termios terminal_local;
    tcgetattr(0, &terminal_local);
    memcpy(&terminal_backup, &terminal_local, sizeof(struct termios));
    cfmakeraw(&terminal_local);
    tcsetattr(0, TCSANOW, &terminal_local);
  }

  virtual void teardown() { tcsetattr(0, TCSANOW, &terminal_backup); }

  virtual TerminalInfo getTerminalInfo() {
    winsize win;
    ioctl(1, TIOCGWINSZ, &win);
    TerminalInfo ti;
    ti.set_row(win.ws_row);
    ti.set_column(win.ws_col);
    ti.set_width(win.ws_xpixel);
    ti.set_height(win.ws_ypixel);
    return ti;
  }

  virtual int getFd() { return STDIN_FILENO; }

 protected:
  termios terminal_backup;

};  // namespace et
}  // namespace et

#endif