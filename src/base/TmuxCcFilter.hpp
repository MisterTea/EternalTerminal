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
 *
 * The client->server direction needs its own classifier: a GUI attached
 * via `tmux -CC` (e.g. iTerm2) does not forward a raw interrupt byte for
 * Ctrl+C/Z/\, it issues a `send-keys` control-mode command instead, either
 * as a hex-encoded byte (`send-keys -H 3`) or a tmux key name (`send-keys
 * C-c`). See {@link tmuxCcContainsInterruptCommand}.
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

/**
 * @brief True for a tmux key name that requests an interrupt (Ctrl+C/Z/\).
 *
 * tmux accepts both the short (`C-c`) and, historically, the caret (`^C`)
 * spellings; both are matched case-insensitively.
 */
inline bool tmuxCcIsInterruptKeyName(const string& token) {
  string t = token;
  for (char& c : t) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }
  return t == "c-c" || t == "^c" || t == "c-z" || t == "^z" || t == "c-\\" ||
         t == "c-|";
}

/**
 * @brief True if @p token is a `send-keys -H` hex byte for Ctrl+C/Z/\.
 *
 * `-H` expects one hex-encoded ASCII byte per argument, so only 1-2 hex
 * digits are accepted.
 */
inline bool tmuxCcHexTokenIsInterruptByte(const string& token) {
  if (token.empty() || token.size() > 2) {
    return false;
  }
  for (char c : token) {
    if (!::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  long value = strtol(token.c_str(), nullptr, 16);
  return value == 0x03 || value == 0x1a || value == 0x1c;
}

/**
 * @brief True if a single tmux control-mode command line asks tmux to send
 * Ctrl+C/Z/\ to a pane, via `send-keys`/`send` (its alias).
 *
 * A GUI client attached with `tmux -CC` (e.g. iTerm2) forwards keystrokes as
 * one of these text commands rather than a raw interrupt byte, so the
 * regular {@link et::WriteBuffer::containsInterruptByte} byte scan never
 * matches. `-H` hex bytes and tmux key names (`C-c`) are recognized; `-t`/
 * `-c`/`-N` are skipped past their argument so a target/repeat-count value
 * is never mistaken for a key.
 */
inline bool tmuxCcLineRequestsInterrupt(const string& rawLine) {
  string line = rawLine;
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  vector<string> tokens;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && ::isspace(static_cast<unsigned char>(line[i]))) {
      ++i;
    }
    size_t start = i;
    while (i < line.size() && !::isspace(static_cast<unsigned char>(line[i]))) {
      ++i;
    }
    if (i > start) {
      tokens.push_back(line.substr(start, i - start));
    }
  }
  if (tokens.empty() || (tokens[0] != "send-keys" && tokens[0] != "send")) {
    return false;
  }
  bool hexMode = false;
  // -l disables key-name lookup entirely: the remaining arguments are
  // literal UTF-8 characters, so a literal "C-c" typed by the user must not
  // be mistaken for the key combination.
  bool literalMode = false;
  for (size_t idx = 1; idx < tokens.size(); ++idx) {
    const string& tok = tokens[idx];
    if (tok == "-H") {
      hexMode = true;
      literalMode = false;
      continue;
    }
    if (!tok.empty() && tok[0] == '-') {
      if (tok == "-l") {
        literalMode = true;
        hexMode = false;
      } else if (tok == "-M" || tok == "-R" || tok == "-X" || tok == "-K" ||
                 tok == "-F") {
        hexMode = false;
      }
      if (tok == "-t" || tok == "-c" || tok == "-N") {
        ++idx;  // Skip the flag's argument so it is never read as a key.
      }
      continue;
    }
    if (literalMode) {
      continue;
    }
    if (hexMode ? tmuxCcHexTokenIsInterruptByte(tok)
                : tmuxCcIsInterruptKeyName(tok)) {
      return true;
    }
  }
  return false;
}

/**
 * @brief True if any line in a client->server control-mode chunk requests
 * an interrupt. See {@link tmuxCcLineRequestsInterrupt}.
 */
inline bool tmuxCcContainsInterruptCommand(const string& data) {
  size_t lineStart = 0;
  for (size_t i = 0; i <= data.size(); ++i) {
    if (i == data.size() || data[i] == '\n') {
      if (i > lineStart &&
          tmuxCcLineRequestsInterrupt(data.substr(lineStart, i - lineStart))) {
        return true;
      }
      lineStart = i + 1;
    }
  }
  return false;
}

}  // namespace et

#endif  // __ET_TMUX_CC_FILTER__
