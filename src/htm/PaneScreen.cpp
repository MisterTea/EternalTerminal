#include "PaneScreen.hpp"

namespace et {
namespace {
void utf8Append(string* out, uint32_t cp) {
  if (cp == 0) {
    return;
  }
  char buf[8];
  int n = 0;
  if (cp < 0x80) {
    buf[n++] = static_cast<char>(cp);
  } else if (cp < 0x800) {
    buf[n++] = static_cast<char>(0xC0 | (cp >> 6));
    buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    buf[n++] = static_cast<char>(0xE0 | (cp >> 12));
    buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    buf[n++] = static_cast<char>(0xF0 | (cp >> 18));
    buf[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
  }
  out->append(buf, static_cast<size_t>(n));
}

string sgrPrefix(const VTermScreenCell& cell) {
  string s = "\x1b[0";
  if (cell.attrs.bold) {
    s += ";1";
  }
  if (cell.attrs.underline) {
    s += ";4";
  }
  if (cell.attrs.italic) {
    s += ";3";
  }
  if (cell.attrs.reverse) {
    s += ";7";
  }
  if (VTERM_COLOR_IS_INDEXED(&cell.fg) &&
      !VTERM_COLOR_IS_DEFAULT_FG(&cell.fg)) {
    int idx = cell.fg.indexed.idx;
    if (idx < 8) {
      s += ";" + to_string(30 + idx);
    } else if (idx < 16) {
      s += ";" + to_string(90 + (idx - 8));
    } else {
      s += ";38;5;" + to_string(idx);
    }
  }
  if (VTERM_COLOR_IS_INDEXED(&cell.bg) &&
      !VTERM_COLOR_IS_DEFAULT_BG(&cell.bg)) {
    int idx = cell.bg.indexed.idx;
    if (idx < 8) {
      s += ";" + to_string(40 + idx);
    } else if (idx < 16) {
      s += ";" + to_string(100 + (idx - 8));
    } else {
      s += ";48;5;" + to_string(idx);
    }
  }
  s += "m";
  return s;
}
}  // namespace

PaneScreen::PaneScreen(int cols, int rows)
    : vt(nullptr),
      screen(nullptr),
      state(nullptr),
      colCount(cols > 0 ? cols : 80),
      rowCount(rows > 0 ? rows : 24),
      altScreen(false),
      cursorOn(true),
      mouseMode(0),
      inputFilterState(InputFilterState::Normal) {
  vt = vterm_new(rowCount, colCount);
  vterm_set_utf8(vt, 1);
  screen = vterm_obtain_screen(vt);
  state = vterm_obtain_state(vt);
  vterm_screen_enable_altscreen(screen, 1);
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.damage = &PaneScreen::onDamage;
  callbacks.settermprop = &PaneScreen::onSetTermProp;
  callbacks.sb_pushline = &PaneScreen::onSbPush;
  callbacks.sb_pushline4 = &PaneScreen::onSbPush4;
  callbacks.sb_clear = &PaneScreen::onSbClear;
  callbacks.movecursor = &PaneScreen::onMoveCursor;
  vterm_screen_set_callbacks(screen, &callbacks, this);
  vterm_screen_callbacks_has_pushline4(screen);
  vterm_screen_reset(screen, 1);
}

PaneScreen::~PaneScreen() {
  if (vt) {
    vterm_free(vt);
  }
}

int PaneScreen::onDamage(VTermRect, void*) { return 1; }

int PaneScreen::onSetTermProp(VTermProp prop, VTermValue* val, void* user) {
  auto* self = static_cast<PaneScreen*>(user);
  switch (prop) {
    case VTERM_PROP_ALTSCREEN:
      self->altScreen = val->boolean != 0;
      break;
    case VTERM_PROP_CURSORVISIBLE:
      self->cursorOn = val->boolean != 0;
      break;
    case VTERM_PROP_MOUSE:
      self->mouseMode = val->number;
      break;
    case VTERM_PROP_TITLE:
      if (val->string.str && val->string.len) {
        if (val->string.initial) {
          self->windowTitle.clear();
        }
        self->windowTitle.append(val->string.str, val->string.len);
      }
      break;
    default:
      break;
  }
  return 1;
}

int PaneScreen::onSbPush(int cols, const VTermScreenCell* cells, void* user) {
  return onSbPush4(cols, cells, false, user);
}

int PaneScreen::onSbPush4(int cols, const VTermScreenCell* cells,
                          bool continuation, void* user) {
  auto* self = static_cast<PaneScreen*>(user);
  HistoryLine line;
  line.cells.assign(cells, cells + cols);
  line.continuation = continuation;
  self->history.push_back(std::move(line));
  while (self->history.size() > kMaxHistory) {
    self->history.pop_front();
  }
  return 1;
}

int PaneScreen::onSbClear(void* user) {
  static_cast<PaneScreen*>(user)->history.clear();
  return 1;
}

int PaneScreen::onMoveCursor(VTermPos, VTermPos, int visible, void* user) {
  static_cast<PaneScreen*>(user)->cursorOn = visible != 0;
  return 1;
}

void PaneScreen::feed(const string& bytes) {
  if (!vt || bytes.empty()) {
    return;
  }
  // screen(1), and therefore TERM=screen shells, use the legacy title
  // sequence ESC k ... ESC \. tmux consumes it as a title update, but
  // libvterm does not and otherwise leaves the title text in the grid.
  // Filter only the screen model; the original bytes still go to %output.
  string filtered;
  filtered.reserve(bytes.size());
  for (unsigned char c : bytes) {
    switch (inputFilterState) {
      case InputFilterState::Normal:
        if (c == '\x1b') {
          inputFilterState = InputFilterState::Escape;
        } else {
          filtered.push_back(static_cast<char>(c));
        }
        break;
      case InputFilterState::Escape:
        if (c == 'k') {
          inputFilterState = InputFilterState::ScreenTitle;
        } else {
          filtered.push_back('\x1b');
          if (c == '\x1b') {
            inputFilterState = InputFilterState::Escape;
          } else {
            filtered.push_back(static_cast<char>(c));
            inputFilterState = InputFilterState::Normal;
          }
        }
        break;
      case InputFilterState::ScreenTitle:
        if (c == '\x1b') {
          inputFilterState = InputFilterState::ScreenTitleEscape;
        } else if (c == '\a') {
          inputFilterState = InputFilterState::Normal;
        }
        break;
      case InputFilterState::ScreenTitleEscape:
        if (c == '\\') {
          inputFilterState = InputFilterState::Normal;
        } else if (c != '\x1b') {
          inputFilterState = InputFilterState::ScreenTitle;
        }
        break;
    }
  }
  if (!filtered.empty()) {
    vterm_input_write(vt, filtered.data(), filtered.size());
  }
  vterm_screen_flush_damage(screen);
}

void PaneScreen::resize(int cols, int rows) {
  colCount = cols > 0 ? cols : 1;
  rowCount = rows > 0 ? rows : 1;
  if (vt) {
    vterm_set_size(vt, rowCount, colCount);
    vterm_screen_flush_damage(screen);
  }
}

void PaneScreen::cursor(int* x, int* y) const {
  VTermPos pos;
  vterm_state_get_cursorpos(state, &pos);
  if (x) {
    *x = pos.col;
  }
  if (y) {
    *y = pos.row;
  }
}

void PaneScreen::readVisible(vector<vector<VTermScreenCell>>* lines) const {
  lines->clear();
  lines->resize(static_cast<size_t>(rowCount));
  for (int r = 0; r < rowCount; r++) {
    (*lines)[static_cast<size_t>(r)].resize(static_cast<size_t>(colCount));
    for (int c = 0; c < colCount; c++) {
      VTermPos pos;
      pos.row = r;
      pos.col = c;
      vterm_screen_get_cell(
          screen, pos,
          &(*lines)[static_cast<size_t>(r)][static_cast<size_t>(c)]);
    }
  }
}

string PaneScreen::cellsToLine(const vector<VTermScreenCell>& cells,
                               bool withEscapes, bool preserveTrailing) const {
  string line;
  string lastSgr;
  int lastNonSpace = -1;
  for (size_t i = 0; i < cells.size(); i++) {
    const VTermScreenCell& cell = cells[i];
    if (withEscapes) {
      string sgr = sgrPrefix(cell);
      if (sgr != lastSgr) {
        line += sgr;
        lastSgr = sgr;
      }
    }
    uint32_t cp = cell.chars[0];
    if (cp == 0) {
      line.push_back(' ');
    } else {
      utf8Append(&line, cp);
      lastNonSpace = static_cast<int>(i);
    }
    if (cell.width > 1) {
      i += static_cast<size_t>(cell.width - 1);
    }
  }
  if (!preserveTrailing && lastNonSpace >= 0) {
    // Trim trailing spaces that were empty cells.
    while (!line.empty() && line.back() == ' ') {
      line.pop_back();
    }
  } else if (!preserveTrailing && lastNonSpace < 0) {
    line.clear();
  }
  if (withEscapes && !line.empty()) {
    line += "\x1b[0m";
  }
  return line;
}

string PaneScreen::capture(bool withEscapes, bool alt, int startLine,
                           int endLine, bool joinWrap,
                           bool preserveTrailing) const {
  (void)joinWrap;
  if (alt && !altScreen) {
    return "";
  }
  vector<vector<VTermScreenCell>> visible;
  readVisible(&visible);

  vector<string> lines;
  vector<bool> continuations;
  for (const auto& histLine : history) {
    lines.push_back(cellsToLine(histLine.cells, withEscapes, preserveTrailing));
    continuations.push_back(histLine.continuation);
  }
  for (size_t row = 0; row < visible.size(); row++) {
    const auto& vis = visible[row];
    lines.push_back(cellsToLine(vis, withEscapes, preserveTrailing));
    const VTermLineInfo* info =
        vterm_state_get_lineinfo(state, static_cast<int>(row));
    continuations.push_back(info && info->continuation);
  }

  int hist = static_cast<int>(history.size());
  int total = static_cast<int>(lines.size());
  int start = startLine;
  int end = endLine;
  // tmux: -S -N starts N lines before the bottom of the history+screen.
  if (start < 0) {
    start = total + start;
  } else {
    start = hist + start;
  }
  if (end < 0) {
    end = total + end;
  } else if (end == 0 && startLine == 0 && endLine == 0) {
    // default: visible screen only
    start = hist;
    end = total - 1;
  } else {
    end = hist + end;
  }
  if (start < 0) {
    start = 0;
  }
  if (end >= total) {
    end = total - 1;
  }
  if (end < start) {
    return "";
  }

  string out;
  for (int i = start; i <= end; i++) {
    if (!out.empty()) {
      if (!joinWrap || !continuations[static_cast<size_t>(i)]) {
        out.push_back('\n');
      }
    }
    out += lines[static_cast<size_t>(i)];
  }
  if (!out.empty()) {
    out.push_back('\n');
  }
  return out;
}

}  // namespace et
