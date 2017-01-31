#ifndef __NCURSES_OVERLAY_H__
#define __NCURSES_OVERLAY_H__

#include "Headers.hpp"

#include <ncurses.h>
#include "ETerminal.pb.h"
#include "StdIoBuffer.hpp"

namespace et {
class NCursesWindow {
 public:
  NCursesWindow(const TerminalInfo& info, bool showBorder);

  ~NCursesWindow();

  void drawTextCentered(string text, int row);

  void refresh();

 protected:
  const TerminalInfo info;
  bool showBorder;
  WINDOW* window;
};

class NCursesOverlay {
 public:
  NCursesOverlay();

  ~NCursesOverlay();

  shared_ptr<NCursesWindow> createWindow(const TerminalInfo& info,
                                         bool showBorder);

  void refresh();

  inline int rows() {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    return rows;
  }
  inline int cols() {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    return cols;
  }

 protected:
  vector<shared_ptr<NCursesWindow> > windows;
  shared_ptr<StdIoBuffer> stdIoBuffer;
};
}

#endif  // __NCURSES_OVERLAY_H__
