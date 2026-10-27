#ifndef __HTM_PANE_SCREEN_H__
#define __HTM_PANE_SCREEN_H__

#include "Headers.hpp"

#ifdef small
// rpcndr.h on Windows defines `small` as `char`, which breaks libvterm's
// VTermScreenCellAttrs bitfield.
#undef small
#endif
extern "C" {
#include "vterm.h"
}

namespace et {

/**
 * @brief libvterm-backed screen + scrollback for capture-pane / list-panes.
 */
class PaneScreen {
 public:
  PaneScreen(int cols, int rows);
  ~PaneScreen();
  PaneScreen(const PaneScreen&) = delete;
  PaneScreen& operator=(const PaneScreen&) = delete;

  void feed(const string& bytes);
  void resize(int cols, int rows);
  int cols() const { return colCount; }
  int rows() const { return rowCount; }
  void cursor(int* x, int* y) const;
  bool alternateScreen() const { return altScreen; }
  bool cursorVisible() const { return cursorOn; }
  int mouseFlags() const { return mouseMode; }
  string title() const { return windowTitle; }

  string capture(bool withEscapes, bool alt, int startLine, int endLine,
                 bool joinWrap, bool preserveTrailing) const;

 private:
  static int onDamage(VTermRect rect, void* user);
  static int onSetTermProp(VTermProp prop, VTermValue* val, void* user);
  static int onSbPush(int cols, const VTermScreenCell* cells, void* user);
  static int onSbPush4(int cols, const VTermScreenCell* cells,
                       bool continuation, void* user);
  static int onSbClear(void* user);
  static int onMoveCursor(VTermPos pos, VTermPos oldpos, int visible,
                          void* user);

  string cellsToLine(const vector<VTermScreenCell>& cells, bool withEscapes,
                     bool preserveTrailing) const;
  void readVisible(vector<vector<VTermScreenCell>>* lines) const;

  VTerm* vt;
  VTermScreen* screen;
  VTermState* state;
  VTermScreenCallbacks callbacks;
  int colCount;
  int rowCount;
  bool altScreen;
  bool cursorOn;
  int mouseMode;
  string windowTitle;
  enum class InputFilterState {
    Normal,
    Escape,
    ScreenTitle,
    ScreenTitleEscape,
  };
  InputFilterState inputFilterState;
  struct HistoryLine {
    vector<VTermScreenCell> cells;
    bool continuation = false;
  };
  deque<HistoryLine> history;
  static const size_t kMaxHistory = 2000;
};

}  // namespace et

#endif
