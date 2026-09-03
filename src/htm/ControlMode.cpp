#include "ControlMode.hpp"

#include <ctime>

namespace et {
namespace {
bool isFlagWithValue(const string& name, char f, const string& next) {
  (void)next;
  if (name == "refresh-client" || name == "refresh") {
    return f == 'C' || f == 'A' || f == 'B' || f == 'f' || f == 'F' ||
           f == 'r' || f == 't';
  }
  if (name == "capture-pane" || name == "capturep") {
    return f == 't' || f == 'S' || f == 'E' || f == 'b';
  }
  if (name == "send-keys" || name == "send") {
    return f == 't';
  }
  if (name == "display-message" || name == "display") {
    return f == 't';
  }
  if (name == "list-windows" || name == "lsw" || name == "list-panes" ||
      name == "lsp" || name == "list-sessions" || name == "list-session" ||
      name == "ls") {
    return f == 'F' || f == 't' || f == 'f';
  }
  if (name == "new-window" || name == "neww" || name == "split-window" ||
      name == "splitw") {
    return f == 't' || f == 'c' || f == 'F' || f == 'n' || f == 'l';
  }
  if (name == "kill-pane" || name == "killp" || name == "kill-window" ||
      name == "killw" || name == "select-pane" || name == "selectp" ||
      name == "select-window" || name == "selectw" || name == "resize-pane" ||
      name == "resizep" || name == "resize-window" || name == "resizew" ||
      name == "rename-window" || name == "renamew" || name == "select-layout" ||
      name == "selectl") {
    return f == 't' || f == 'T' || f == 'x' || f == 'y';
  }
  if (name == "attach-session" || name == "attach" || name == "new-session" ||
      name == "new" || name == "kill-session" || name == "rename-session" ||
      name == "rename") {
    return f == 't' || f == 's' || f == 'n' || f == 'c';
  }
  if (name == "show-buffer" || name == "showb" || name == "set-buffer" ||
      name == "setb") {
    return f == 'b';
  }
  if (name == "swap-pane" || name == "swapp" || name == "move-pane" ||
      name == "movep" || name == "break-pane" || name == "breakp" ||
      name == "join-pane" || name == "joinp") {
    return f == 's' || f == 't' || f == 'F';
  }
  if (name == "copy-mode") {
    return f == 't';
  }
  if (name == "show-options" || name == "show" || name == "show-option" ||
      name == "set-option" || name == "set" || name == "show-window-options" ||
      name == "showw" || name == "set-window-option" || name == "setw") {
    return f == 't' || f == 'o';
  }
  if (name == "unlink-window" || name == "unlinkw" || name == "link-window" ||
      name == "linkw" || name == "move-window" || name == "movew") {
    return f == 's' || f == 't';
  }
  return f == 't' || f == 'F' || f == 'c' || f == 's' || f == 'n' || f == 'b';
}

bool flagAttachedToToken(char /*f*/) { return true; }
}  // namespace

string controlOctalEscape(const string& data) {
  string out;
  out.reserve(data.size() * 2);
  for (unsigned char c : data) {
    if (c < 32 || c == '\\') {
      char buf[5];
      snprintf(buf, sizeof(buf), "\\%03o", c);
      out.append(buf);
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

vector<string> controlSplitArgs(const string& line) {
  vector<string> args;
  string cur;
  bool inSingle = false;
  bool inDouble = false;
  bool escaped = false;
  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (escaped) {
      cur.push_back(c);
      escaped = false;
      continue;
    }
    if (!inSingle && c == '\\') {
      escaped = true;
      continue;
    }
    if (!inDouble && c == '\'') {
      inSingle = !inSingle;
      continue;
    }
    if (!inSingle && c == '"') {
      inDouble = !inDouble;
      continue;
    }
    if (!inSingle && !inDouble && isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) {
        args.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur.push_back(c);
  }
  if (!cur.empty()) {
    args.push_back(cur);
  }
  return args;
}

vector<string> splitControlCommandList(const string& line) {
  vector<string> cmds;
  string cur;
  bool inSingle = false;
  bool inDouble = false;
  bool escaped = false;
  bool sawSep = false;
  auto pushCmd = [&]() {
    size_t start = 0;
    size_t end = cur.size();
    while (start < end && isspace(static_cast<unsigned char>(cur[start]))) {
      start++;
    }
    while (end > start && isspace(static_cast<unsigned char>(cur[end - 1]))) {
      end--;
    }
    if (end > start) {
      cmds.push_back(cur.substr(start, end - start));
    }
    cur.clear();
  };
  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (escaped) {
      cur.push_back(c);
      escaped = false;
      continue;
    }
    if (!inSingle && c == '\\') {
      escaped = true;
      continue;
    }
    if (!inDouble && c == '\'') {
      inSingle = !inSingle;
      cur.push_back(c);
      continue;
    }
    if (!inSingle && c == '"') {
      inDouble = !inDouble;
      cur.push_back(c);
      continue;
    }
    if (!inSingle && !inDouble && c == ';') {
      sawSep = true;
      pushCmd();
      continue;
    }
    cur.push_back(c);
  }
  if (!sawSep) {
    return {line};
  }
  pushCmd();
  return cmds;
}

ParsedControlCommand parseControlCommand(const string& line) {
  ParsedControlCommand parsed;
  vector<string> args = controlSplitArgs(line);
  if (args.empty()) {
    return parsed;
  }
  parsed.name = args[0];
  for (size_t i = 1; i < args.size(); i++) {
    const string& tok = args[i];
    if (tok == "--") {
      parsed.flags.positional.insert(parsed.flags.positional.end(),
                                     args.begin() + static_cast<long>(i) + 1,
                                     args.end());
      break;
    }
    if (tok.size() >= 2 && tok[0] == '-' && tok[1] != '-') {
      for (size_t j = 1; j < tok.size(); j++) {
        char f = tok[j];
        parsed.flags.present.insert(f);
        if (j + 1 < tok.size() && flagAttachedToToken(f) &&
            isFlagWithValue(parsed.name, f, tok.substr(j + 1))) {
          // -t%1 or -C80x24
          parsed.flags.values[f].push_back(tok.substr(j + 1));
          break;
        }
        if (isFlagWithValue(parsed.name, f,
                            i + 1 < args.size() ? args[i + 1] : string()) &&
            j + 1 == tok.size() && i + 1 < args.size() &&
            (args[i + 1].empty() || args[i + 1][0] != '-' ||
             args[i + 1].size() == 1)) {
          parsed.flags.values[f].push_back(args[++i]);
          break;
        }
      }
    } else {
      parsed.flags.positional.push_back(tok);
    }
  }
  return parsed;
}

ControlWriter::ControlWriter()
    : socketHandler(),
      fd(-1),
      cmdNumber(0),
      replyTime(0),
      replyFlags(1),
      inReply(false) {}

void ControlWriter::setSocket(shared_ptr<SocketHandler> handler, int endpoint) {
  socketHandler = handler;
  fd = endpoint;
}

void ControlWriter::clearSocket() {
  socketHandler.reset();
  fd = -1;
  inReply = false;
  pendingNotify.clear();
}

void ControlWriter::writeLine(const string& line) {
  if (!hasClient()) {
    return;
  }
  string payload = line;
  if (payload.empty() || payload.back() != '\n') {
    payload.push_back('\n');
  }
  socketHandler->writeAllOrThrow(fd, payload.data(),
                                 static_cast<int>(payload.size()), false);
}

void ControlWriter::begin() { beginWithFlags(1); }

void ControlWriter::beginServerOriginated() { beginWithFlags(0); }

void ControlWriter::beginWithFlags(int flags) {
  cmdNumber++;
  inReply = true;
  replyFlags = flags;
  replyTime = static_cast<long long>(time(nullptr));
  writeGuard("%begin");
}

void ControlWriter::writeGuard(const char* kind) {
  writeLine(string(kind) + " " + to_string(replyTime) + " " +
            to_string(cmdNumber) + " " + to_string(replyFlags));
}

void ControlWriter::writeOutput(const string& text) {
  if (text.empty() || !hasClient()) {
    return;
  }
  socketHandler->writeAllOrThrow(fd, text.data(), static_cast<int>(text.size()),
                                 false);
  if (text.back() != '\n') {
    socketHandler->writeAllOrThrow(fd, "\n", 1, false);
  }
}

void ControlWriter::end() {
  writeGuard("%end");
  inReply = false;
  flushNotifications();
}

void ControlWriter::error(const string& message) {
  if (!inReply) {
    begin();
  }
  writeOutput(message);
  writeGuard("%error");
  inReply = false;
  flushNotifications();
}

void ControlWriter::notify(const string& line) {
  if (inReply) {
    pendingNotify.push_back(line);
    return;
  }
  writeLine(line);
}

void ControlWriter::flushNotifications() {
  for (const string& line : pendingNotify) {
    writeLine(line);
  }
  pendingNotify.clear();
}

}  // namespace et
