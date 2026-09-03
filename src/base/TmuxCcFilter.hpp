#ifndef __ET_TMUX_CC_FILTER__
#define __ET_TMUX_CC_FILTER__

#include "Headers.hpp"

namespace et {

/**
 * @brief Keep/drop classifier for a tmux control-mode byte stream.
 *
 * Control mode is line-oriented. Pane stdout arrives as complete
 * `%output` / `%extended-output` lines (octal-escaped). Those are
 * droppable on interrupt, like a TTY flood. Session/window/layout
 * notifications and `%begin`…`%end`/`%error` blocks must be kept or
 * a nested `tmux -CC` client desyncs.
 *
 * Incomplete trailing bytes: keep a control notification that has not
 * seen its newline yet; drop an incomplete `%output` line and skip until
 * the next newline so the rest of that line cannot reappear as a new
 * message. Incomplete TTY is dropped without that skip so a following
 * prompt is not discarded.
 */
struct TmuxCcFilterResult {
  string kept;
  size_t dropped = 0;
  bool skipUntilNewline = false;
};

inline string tmuxCcFirstToken(const string& line) {
  size_t end = line.find_first_of(" \t");
  if (end == string::npos) {
    return line;
  }
  return line.substr(0, end);
}

inline bool tmuxCcIsDroppableOutputToken(const string& token) {
  return token == "%output" || token == "%extended-output";
}

inline bool tmuxCcShouldKeepLine(const string& line, bool* inBeginBlock) {
  string token = tmuxCcFirstToken(line);
  if (*inBeginBlock) {
    if (token == "%end" || token == "%error") {
      *inBeginBlock = false;
    }
    return true;
  }
  if (token == "%begin") {
    *inBeginBlock = true;
    return true;
  }
  return !token.empty() && token[0] == '%' &&
         !tmuxCcIsDroppableOutputToken(token);
}

inline TmuxCcFilterResult filterTmuxCc(const string& data) {
  TmuxCcFilterResult result;
  bool inBeginBlock = false;
  size_t lineStart = 0;
  for (size_t i = 0; i < data.size(); ++i) {
    if (data[i] != '\n') {
      continue;
    }
    string line = data.substr(lineStart, i - lineStart);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const size_t rawLen = i + 1 - lineStart;
    if (tmuxCcShouldKeepLine(line, &inBeginBlock)) {
      result.kept.append(data, lineStart, rawLen);
    } else {
      result.dropped += rawLen;
    }
    lineStart = i + 1;
  }
  if (lineStart >= data.size()) {
    return result;
  }

  string tail = data.substr(lineStart);
  string line = tail;
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  string token = tmuxCcFirstToken(line);
  bool keep = false;
  if (inBeginBlock) {
    keep = true;
  } else if (tmuxCcIsDroppableOutputToken(token)) {
    result.skipUntilNewline = true;
  } else if (!token.empty() && token[0] == '%') {
    keep = true;
  }
  if (keep) {
    result.kept.append(tail);
  } else {
    result.dropped += tail.size();
  }
  return result;
}

}  // namespace et

#endif  // __ET_TMUX_CC_FILTER__
