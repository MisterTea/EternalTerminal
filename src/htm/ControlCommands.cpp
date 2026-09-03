#include "ControlCommands.hpp"

namespace et {
namespace {
string keyToken(const string& tok) {
  if (tok == "Enter" || tok == "C-m" || tok == "KPEnter") {
    return "\r";
  }
  if (tok == "Escape" || tok == "C-[" || tok == "Esc") {
    return "\x1b";
  }
  if (tok == "Space") {
    return " ";
  }
  if (tok == "Tab" || tok == "C-i") {
    return "\t";
  }
  if (tok == "BSpace" || tok == "Bspace") {
    return "\x7f";
  }
  if (tok.size() == 3 && tok[0] == 'C' && tok[1] == '-') {
    char c = tok[2];
    if (c >= 'a' && c <= 'z') {
      return string(1, static_cast<char>(c - 'a' + 1));
    }
    if (c >= 'A' && c <= 'Z') {
      return string(1, static_cast<char>(c - 'A' + 1));
    }
  }
  // tmux key names: 0x0d, 0xd, 0x20
  if (tok.size() >= 3 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
    unsigned int v = 0;
    if (sscanf(tok.c_str(), "%x", &v) == 1) {
      return string(1, static_cast<char>(v & 0xff));
    }
  }
  return tok;
}

int parseInt(const string& s, int fallback) {
  if (s.empty()) {
    return fallback;
  }
  try {
    return stoi(s);
  } catch (...) {
    return fallback;
  }
}

void parseSize(const string& spec, int* cols, int* rows, uint32_t* windowId) {
  *windowId = 0;
  string rest = spec;
  if (!rest.empty() && rest[0] == '@') {
    size_t colon = rest.find(':');
    if (colon != string::npos) {
      *windowId = static_cast<uint32_t>(stoul(rest.substr(1, colon - 1)));
      rest = rest.substr(colon + 1);
    }
  }
  size_t x = rest.find('x');
  if (x == string::npos) {
    x = rest.find(',');
  }
  if (x == string::npos) {
    return;
  }
  *cols = parseInt(rest.substr(0, x), *cols);
  *rows = parseInt(rest.substr(x + 1), *rows);
}

void applyClientFlags(MultiplexerState* mux, const string& flags) {
  string cur;
  auto flush = [&]() {
    if (cur.empty()) {
      return;
    }
    bool off = cur[0] == '!';
    string name = off ? cur.substr(1) : cur;
    size_t eq = name.find('=');
    string key = eq == string::npos ? name : name.substr(0, eq);
    string val = eq == string::npos ? string() : name.substr(eq + 1);
    if (key == "no-output") {
      mux->clientFlags.noOutput = !off;
    } else if (key == "wait-exit") {
      mux->clientFlags.waitExit = !off;
    } else if (key == "pause-after") {
      mux->clientFlags.pauseAfterSec = off ? -1 : parseInt(val, 0);
    }
    cur.clear();
  };
  for (char c : flags) {
    if (c == ',') {
      flush();
    } else {
      cur.push_back(c);
    }
  }
  flush();
}

bool isSetShowCommand(const string& name) {
  return name == "set-option" || name == "set" || name == "show-options" ||
         name == "show" || name == "show-option" ||
         name == "set-window-option" || name == "setw" ||
         name == "show-window-options" || name == "showw";
}

bool isWindowOptionCommand(const string& name) {
  return name == "set-window-option" || name == "setw" ||
         name == "show-window-options" || name == "showw";
}

bool isShowCommand(const string& name) {
  return name == "show-options" || name == "show" || name == "show-option" ||
         name == "show-window-options" || name == "showw";
}

char optionScope(const ParsedControlCommand& cmd) {
  if (cmd.flags.has('p')) {
    return 'p';
  }
  if (cmd.flags.has('w') || isWindowOptionCommand(cmd.name)) {
    return 'w';
  }
  if (cmd.flags.has('g')) {
    return 'g';
  }
  if (cmd.flags.has('s')) {
    return 's';
  }
  return ' ';
}

uint32_t optionTarget(MultiplexerState* mux, const ParsedControlCommand& cmd,
                      char scope) {
  switch (scope) {
    case 'p':
      return mux->parsePaneTarget(cmd.flags.get('t'));
    case 'w':
      return mux->parseWindowTarget(cmd.flags.get('t'));
    case 'g':
    case 's':
      return 0;
    default:
      return mux->parseSessionTarget(cmd.flags.get('t'));
  }
}

void executeSetShow(MultiplexerState* mux, ControlWriter* writer,
                    const ParsedControlCommand& cmd) {
  char scope = optionScope(cmd);
  uint32_t target = optionTarget(mux, cmd, scope);
  string name = cmd.flags.get('o');
  string value;
  size_t pos = 0;
  if (name.empty() && pos < cmd.flags.positional.size()) {
    name = cmd.flags.positional[pos++];
  }
  if (pos < cmd.flags.positional.size()) {
    value = cmd.flags.positional[pos];
  }
  writer->begin();
  if (isShowCommand(cmd.name)) {
    if (!name.empty() && name[0] == '@') {
      string got = mux->getUserOption(scope, target, name);
      if (!got.empty() || !cmd.flags.has('q')) {
        if (cmd.flags.has('v')) {
          if (!got.empty()) {
            writer->writeOutput(got);
          }
        } else if (!got.empty()) {
          writer->writeOutput(name + " " + got);
        }
      }
    }
  } else if (cmd.flags.has('u')) {
    mux->unsetUserOption(scope, target, name);
  } else {
    mux->setUserOption(scope, target, name, value, cmd.flags.has('a'));
  }
  writer->end();
}
}  // namespace

string encodeSendKeys(const vector<string>& tokens, bool hex, bool literal) {
  string out;
  for (const string& tok : tokens) {
    if (hex) {
      unsigned int v = 0;
      sscanf(tok.c_str(), "%x", &v);
      out.push_back(static_cast<char>(v));
    } else if (literal) {
      out += tok;
    } else {
      out += keyToken(tok);
    }
  }
  return out;
}

ControlAction executeControlCommand(MultiplexerState* mux,
                                    ControlWriter* writer, const string& line) {
  string trimmed = line;
  while (!trimmed.empty() &&
         (trimmed.back() == '\r' || trimmed.back() == '\n')) {
    trimmed.pop_back();
  }
  if (trimmed.empty()) {
    return ControlAction::Detach;
  }
  ParsedControlCommand cmd = parseControlCommand(trimmed);
  if (cmd.name.empty()) {
    return ControlAction::Detach;
  }

  auto paneTarget = [&]() { return mux->parsePaneTarget(cmd.flags.get('t')); };
  auto windowTarget = [&]() {
    return mux->parseWindowTarget(cmd.flags.get('t'));
  };
  auto sessionTarget = [&]() {
    return mux->parseSessionTarget(cmd.flags.get('t', cmd.flags.get('s')));
  };

  try {
    if (cmd.name == "detach-client" || cmd.name == "detach" ||
        cmd.name == "exit") {
      writer->begin();
      writer->end();
      return ControlAction::Detach;
    }
    if (cmd.name == "kill-server") {
      writer->begin();
      writer->end();
      return ControlAction::KillServer;
    }
    if (cmd.name == "display-message" || cmd.name == "display") {
      string fmt = cmd.flags.positional.empty() ? string("#{pane_id}")
                                                : cmd.flags.positional[0];
      uint32_t pane = 0, window = 0, session = mux->activeSessionId();
      if (cmd.flags.has('t')) {
        string t = cmd.flags.get('t');
        if (!t.empty() && t[0] == '%') {
          pane = mux->parsePaneTarget(t);
        } else if (!t.empty() && t[0] == '@') {
          window = mux->parseWindowTarget(t);
        } else if (!t.empty() && t[0] == '$') {
          session = mux->parseSessionTarget(t);
        }
      }
      writer->begin();
      if (cmd.flags.has('p')) {
        writer->writeOutput(mux->displayFormat(fmt, session, window, pane));
      }
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "list-sessions" || cmd.name == "list-session" ||
        cmd.name == "ls") {
      writer->begin();
      writer->writeOutput(mux->listSessions(cmd.flags.get('F')));
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "list-windows" || cmd.name == "lsw") {
      writer->begin();
      writer->writeOutput(
          mux->listWindows(cmd.flags.get('F'), sessionTarget()));
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "list-panes" || cmd.name == "lsp") {
      uint32_t target = 0;
      if (cmd.flags.has('t')) {
        string t = cmd.flags.get('t');
        if (!t.empty() && t[0] == '%') {
          target = mux->parsePaneTarget(t);
        } else {
          target = mux->parseWindowTarget(t);
        }
      } else {
        target = mux->activeWindowId();
      }
      writer->begin();
      if (cmd.flags.has('a')) {
        writer->writeOutput(mux->listAllPanes(cmd.flags.get('F')));
      } else {
        writer->writeOutput(mux->listPanes(cmd.flags.get('F'), target));
      }
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "new-window" || cmd.name == "neww") {
      string cwd = cmd.flags.get('c');
      string name = cmd.flags.get('n');
      writer->begin();
      uint32_t id = mux->newWindow(name, cwd);
      if (cmd.flags.has('P')) {
        string fmt = cmd.flags.get('F', "#{window_id}");
        writer->writeOutput(
            mux->displayFormat(fmt, mux->activeSessionId(), id, 0));
      }
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "split-window" || cmd.name == "splitw") {
      bool stacked = !cmd.flags.has('h');
      if (cmd.flags.has('v')) {
        stacked = true;
      }
      uint32_t src = paneTarget();
      writer->begin();
      uint32_t id = mux->splitWindow(src, stacked, cmd.flags.get('c'));
      if (cmd.flags.has('P')) {
        string fmt = cmd.flags.get('F', "#{pane_id}");
        writer->writeOutput(
            mux->displayFormat(fmt, mux->activeSessionId(), 0, id));
      }
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "kill-pane" || cmd.name == "killp") {
      writer->begin();
      mux->closePane(paneTarget());
      writer->end();
      return mux->empty() ? ControlAction::KillServer : ControlAction::None;
    }
    if (cmd.name == "kill-window" || cmd.name == "killw") {
      writer->begin();
      mux->closeWindow(windowTarget());
      writer->end();
      return mux->empty() ? ControlAction::KillServer : ControlAction::None;
    }
    if (cmd.name == "send-keys" || cmd.name == "send") {
      writer->begin();
      mux->sendKeys(paneTarget(),
                    encodeSendKeys(cmd.flags.positional, cmd.flags.has('H'),
                                   cmd.flags.has('l')));
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "select-pane" || cmd.name == "selectp") {
      writer->begin();
      if (cmd.flags.has('T')) {
        mux->setPaneTitle(paneTarget(), cmd.flags.get('T'));
      } else {
        mux->selectPane(paneTarget());
      }
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "select-window" || cmd.name == "selectw") {
      writer->begin();
      mux->selectWindow(windowTarget());
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "resize-pane" || cmd.name == "resizep") {
      uint32_t pane = paneTarget();
      int amount = cmd.flags.positional.empty()
                       ? 1
                       : parseInt(cmd.flags.positional[0], 1);
      writer->begin();
      if (cmd.flags.has('Z')) {
        mux->zoomToggle(pane);
      } else if (cmd.flags.has('L')) {
        mux->resizePaneDir(pane, 'L', amount);
      } else if (cmd.flags.has('R')) {
        mux->resizePaneDir(pane, 'R', amount);
      } else if (cmd.flags.has('U')) {
        mux->resizePaneDir(pane, 'U', amount);
      } else if (cmd.flags.has('D')) {
        mux->resizePaneDir(pane, 'D', amount);
      }
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "resize-window" || cmd.name == "resizew") {
      uint32_t window = windowTarget();
      int cols = parseInt(cmd.flags.get('x'), mux->clientCols());
      int rows = parseInt(cmd.flags.get('y'), mux->clientRows());
      writer->begin();
      mux->setWindowSize(window, cols, rows);
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "refresh-client" || cmd.name == "refresh") {
      writer->begin();
      if (cmd.flags.has('C')) {
        int cols = mux->clientCols();
        int rows = mux->clientRows();
        uint32_t wid = 0;
        parseSize(cmd.flags.get('C'), &cols, &rows, &wid);
        if (wid) {
          mux->setWindowSize(wid, cols, rows);
        } else {
          mux->setClientSize(cols, rows);
        }
      }
      if (cmd.flags.has('f') || cmd.flags.has('F')) {
        applyClientFlags(mux, cmd.flags.get('f', cmd.flags.get('F')));
      }
      for (const string& spec : cmd.flags.all('A')) {
        size_t colon = spec.find(':');
        if (colon == string::npos) {
          continue;
        }
        uint32_t pane = mux->parsePaneTarget(spec.substr(0, colon));
        string state = spec.substr(colon + 1);
        mux->clientFlags.paneGate[pane] = state;
        if (state == "continue") {
          writer->notify("%continue %" + to_string(pane));
        }
      }
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "capture-pane" || cmd.name == "capturep") {
      uint32_t pane = paneTarget();
      int start = parseInt(cmd.flags.get('S'), 0);
      int end = parseInt(cmd.flags.get('E'), 0);
      bool pending = cmd.flags.has('P') && cmd.flags.has('C');
      writer->begin();
      if (cmd.flags.has('p') && !pending) {
        writer->writeOutput(mux->capturePane(
            pane, cmd.flags.has('e'), cmd.flags.has('a'), start, end,
            cmd.flags.has('J'), cmd.flags.has('N')));
      }
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "rename-window" || cmd.name == "renamew") {
      string name =
          cmd.flags.positional.empty() ? string() : cmd.flags.positional[0];
      writer->begin();
      mux->renameWindow(windowTarget(), name);
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "select-layout" || cmd.name == "selectl") {
      string layout = cmd.flags.positional.empty() ? string("tiled")
                                                   : cmd.flags.positional[0];
      writer->begin();
      mux->selectLayout(windowTarget(), layout);
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "swap-pane" || cmd.name == "swapp") {
      uint32_t dst = mux->parsePaneTarget(cmd.flags.get('t'));
      string srcSpec = cmd.flags.get('s');
      uint32_t src =
          srcSpec.empty() ? mux->activePaneId() : mux->parsePaneTarget(srcSpec);
      writer->begin();
      mux->swapPanes(src, dst);
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "move-pane" || cmd.name == "movep" ||
        cmd.name == "join-pane" || cmd.name == "joinp") {
      uint32_t dst = mux->parsePaneTarget(cmd.flags.get('t'));
      string srcSpec = cmd.flags.get('s');
      uint32_t src =
          srcSpec.empty() ? mux->activePaneId() : mux->parsePaneTarget(srcSpec);
      bool stacked = !cmd.flags.has('h');
      if (cmd.flags.has('v')) {
        stacked = true;
      }
      writer->begin();
      mux->movePane(src, dst, stacked, cmd.flags.has('b'));
      writer->end();
      return mux->empty() ? ControlAction::KillServer : ControlAction::None;
    }
    if (cmd.name == "break-pane" || cmd.name == "breakp") {
      string spec = cmd.flags.get('s');
      if (spec.empty()) {
        spec = cmd.flags.get('t');
      }
      if (spec.empty() && !cmd.flags.positional.empty()) {
        spec = cmd.flags.positional[0];
      }
      uint32_t pane = mux->parsePaneTarget(spec);
      writer->begin();
      uint32_t wid = mux->breakPane(pane);
      if (cmd.flags.has('P')) {
        string fmt = cmd.flags.get('F', "#{window_id}");
        writer->writeOutput(
            mux->displayFormat(fmt, mux->activeSessionId(), wid, 0));
      }
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "unlink-window" || cmd.name == "unlinkw") {
      writer->begin();
      mux->closeWindow(windowTarget());
      writer->end();
      return mux->empty() ? ControlAction::KillServer : ControlAction::None;
    }
    if (cmd.name == "link-window" || cmd.name == "linkw") {
      writer->begin();
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "move-window" || cmd.name == "movew") {
      uint32_t wid = mux->parseWindowTarget(cmd.flags.get('s'));
      uint32_t sid = mux->parseSessionTarget(cmd.flags.get('t'));
      writer->begin();
      mux->moveWindowToSession(wid, sid);
      writer->end();
      return ControlAction::None;
    }
    if (isSetShowCommand(cmd.name)) {
      executeSetShow(mux, writer, cmd);
      return ControlAction::None;
    }
    if (cmd.name == "new-session" || cmd.name == "new") {
      writer->begin();
      uint32_t id = mux->newSession(cmd.flags.get('s'));
      if (!cmd.flags.has('d')) {
        mux->attachSession(id);
      }
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "kill-session") {
      writer->begin();
      mux->closeSession(sessionTarget());
      writer->end();
      return mux->empty() ? ControlAction::KillServer : ControlAction::None;
    }
    if (cmd.name == "rename-session" || cmd.name == "rename") {
      string name =
          cmd.flags.positional.empty() ? string() : cmd.flags.positional[0];
      writer->begin();
      mux->renameSession(sessionTarget(), name);
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "attach-session" || cmd.name == "attach") {
      writer->begin();
      mux->attachSession(sessionTarget());
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "show-buffer" || cmd.name == "showb") {
      string name = cmd.flags.get('b', "buffer0");
      writer->begin();
      auto it = mux->pasteBuffers.find(name);
      if (it != mux->pasteBuffers.end()) {
        writer->writeOutput(it->second);
      }
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "set-buffer" || cmd.name == "setb") {
      string name = cmd.flags.get('b', "buffer0");
      string value =
          cmd.flags.positional.empty() ? string() : cmd.flags.positional[0];
      writer->begin();
      mux->pasteBuffers[name] = value;
      if (writer) {
        writer->notify("%paste-buffer-changed " + name);
      }
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "list-commands" || cmd.name == "lscm") {
      // WezTerm probes this after %session-changed. Include refresh-client
      // with -C XxY so it can size the client the way modern tmux does.
      writer->begin();
      writer->writeOutput(
          "kill-pane [-a] [-t target-pane]\n"
          "new-window [-adkP] [-c start-directory] [-n window-name] "
          "[-t target-window]\n"
          "refresh-client [-C XxY] [-t target-client]\n"
          "resize-pane [-DLRTUZ] [-x width] [-y height] [-t target-pane]\n"
          "resize-window [-aADLTUx] [-x width] [-y height] [-t target-window]\n"
          "send-keys [-Hl] [-t target-pane] key ...\n"
          "split-window [-bdfhvP] [-c start-directory] [-t target-pane]");
      writer->end();
      return ControlAction::None;
    }
    if (cmd.name == "copy-mode" || cmd.name == "list-keys" ||
        cmd.name == "lsk" || cmd.name == "list-clients" || cmd.name == "lsc" ||
        cmd.name == "phony-command" || cmd.name == "clear-history" ||
        cmd.name == "clearhist") {
      writer->begin();
      writer->end();
      return ControlAction::None;
    }
    writer->error("parse error: unknown command: " + cmd.name);
    return ControlAction::Error;
  } catch (const std::exception& ex) {
    writer->error(string("error: ") + ex.what());
    return ControlAction::Error;
  }
  return ControlAction::None;
}

}  // namespace et
