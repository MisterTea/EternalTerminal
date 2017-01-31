#include "NCursesOverlay.hpp"

#include "StdIoBuffer.hpp"

namespace et {
NCursesWindow::NCursesWindow(const TerminalInfo& _info, bool _showBorder)
    : info(_info), showBorder(_showBorder) {
  window = newwin(info.height(), info.width(), info.row(), info.column());
  if (showBorder) {
    box(window, 0, 0); /* 0, 0 gives default characters
                        * for the vertical and horizontal
                        * lines                        */
  }
  refresh();
}

NCursesWindow::~NCursesWindow() {
  if (showBorder) {
    wborder(window, ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ');
  }
  refresh();
  delwin(window);
}

void NCursesWindow::drawTextCentered(string text, int row) {
  int drawcol = max(0, info.width() / 2 - int(text.length()) / 2);
  mvwprintw(window, row, drawcol, "%s", text.c_str());
}

void NCursesWindow::refresh() { wrefresh(window); }

NCursesOverlay::NCursesOverlay() {
  stdIoBuffer = shared_ptr<StdIoBuffer>(new StdIoBuffer());
  initscr();
  curs_set(0);
  refresh();
}

NCursesOverlay::~NCursesOverlay() {
  for (const auto& it : windows) {
    if (it.use_count() != 1) {
      LOG(FATAL)
          << "Tried to delete the overlay with a dangling window reference";
    }
  }
  windows.clear();
  curs_set(1);
  ::refresh();
  endwin();
  stdIoBuffer.reset();
}

shared_ptr<NCursesWindow> NCursesOverlay::createWindow(const TerminalInfo& info,
                                                       bool showBorder) {
  auto retval = shared_ptr<NCursesWindow>(new NCursesWindow(info, showBorder));
  windows.push_back(retval);
  return retval;
}

void NCursesOverlay::refresh() {
  ::refresh();
  for (auto it : windows) {
    it->refresh();
  }
}
}
