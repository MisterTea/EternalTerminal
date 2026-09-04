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
 * message. If a `%output` line was already partly sent, keep through its
 * newline so the client never sees a non-`%` fragment (iTerm2 disconnects
 * on those). Incomplete TTY is dropped without that skip so a following
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
  string droppable;
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

inline bool tmuxCcShouldKeepLine(const string& line, bool* inBeginBlock,
                                 bool inControlMode) {
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
  if (token.empty()) {
    // Keep a resync newline after a mid-line `%output` cut so a later
    // filter pass cannot swallow the line terminator the client needs.
    return inControlMode;
  }
  return token[0] == '%' && !tmuxCcIsDroppableOutputToken(token);
}

inline bool tmuxCcStartsAtLineBoundary(const string& data) {
  return data.empty() || data[0] == '%' || data[0] == '\n';
}

inline TmuxCcFilterResult filterTmuxCc(const string& data,
                                       bool inControlMode = false,
                                       bool discarding = true) {
  if (!data.empty() && inControlMode && !tmuxCcStartsAtLineBoundary(data)) {
    // A %output / %extended-output line was already partly sent. Cutting it
    // leaves a non-`%` fragment as the next line; iTerm2 treats that as an
    // unrecognized command and tears down tmux mode. Finish this line, then
    // filter complete messages as usual.
    size_t newline = data.find('\n');
    if (newline == string::npos) {
      TmuxCcFilterResult result;
      result.kept = data;
      return result;
    }
    TmuxCcFilterResult rest =
        filterTmuxCc(data.substr(newline + 1), true, discarding);
    TmuxCcFilterResult result;
    result.kept.assign(data, 0, newline + 1);
    result.kept += rest.kept;
    result.droppable = rest.droppable;
    result.dropped = rest.dropped;
    result.skipUntilNewline = rest.skipUntilNewline;
    return result;
  }

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
    if (tmuxCcShouldKeepLine(line, &inBeginBlock, inControlMode)) {
      result.kept.append(data, lineStart, rawLen);
    } else {
      result.droppable.append(data, lineStart, rawLen);
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
    if (discarding) {
      result.skipUntilNewline = true;
    }
  } else if (!token.empty() && token[0] == '%') {
    keep = true;
  }
  if (keep) {
    result.kept.append(tail);
  } else {
    result.droppable.append(tail);
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
  string hex = token;
  if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
    hex = hex.substr(2);
  }
  if (hex.empty() || hex.size() > 2) {
    return false;
  }
  for (char c : hex) {
    if (!::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  long value = strtol(hex.c_str(), nullptr, 16);
  return value == 0x03 || value == 0x1a || value == 0x1c;
}

/**
 * @brief True for iTerm2's `send -t %pane 0x03` encoding (no `-H` flag).
 *
 * Bare `3`/`03` without `0x` is not treated as Ctrl+C: that is the digit
 * key unless `-H` is set.
 */
inline bool tmuxCcHexLiteralIsInterruptByte(const string& token) {
  return token.size() >= 3 && token[0] == '0' &&
         (token[1] == 'x' || token[1] == 'X') &&
         tmuxCcHexTokenIsInterruptByte(token);
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
    if (!tok.empty() && tok[0] == '-') {
      const bool combined = tok.size() > 2 && tok[1] != '-';
      if (tok == "-H" || (combined && tok.find('H') != string::npos)) {
        hexMode = true;
        literalMode = false;
      }
      if (tok == "-l" || (combined && tok.find('l') != string::npos &&
                          tok.find('H') == string::npos)) {
        literalMode = true;
        hexMode = false;
      } else if (tok == "-M" || tok == "-R" || tok == "-X" || tok == "-K" ||
                 tok == "-F") {
        hexMode = false;
      }
      if (tok == "-t" || tok == "-c" || tok == "-N" ||
          (combined &&
           (tok.find('t') != string::npos || tok.find('c') != string::npos ||
            tok.find('N') != string::npos))) {
        ++idx;  // Skip the flag's argument so it is never read as a key.
      }
      continue;
    }
    if (literalMode) {
      continue;
    }
    if (hexMode ? tmuxCcHexTokenIsInterruptByte(tok)
                : (tmuxCcIsInterruptKeyName(tok) ||
                   tmuxCcHexLiteralIsInterruptByte(tok))) {
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
    const bool atEnd = i == data.size();
    const bool newline = !atEnd && (data[i] == '\n' || data[i] == '\r');
    if (atEnd || newline) {
      if (i > lineStart &&
          tmuxCcLineRequestsInterrupt(data.substr(lineStart, i - lineStart))) {
        return true;
      }
      if (!atEnd && data[i] == '\r' && i + 1 < data.size() &&
          data[i + 1] == '\n') {
        ++i;
      }
      lineStart = i + 1;
    }
  }
  return false;
}

/**
 * @brief True if @p previousIncomplete plus @p chunk contains a send-keys
 * interrupt, including when the command is split across two reads.
 */
inline bool tmuxCcInputRequestsInterrupt(const string& previousIncomplete,
                                         const string& chunk) {
  return tmuxCcContainsInterruptCommand(previousIncomplete + chunk);
}

/** @brief Keep the trailing incomplete line so the next chunk can finish it. */
inline void tmuxCcRetainIncompleteLine(string* carry, const string& chunk,
                                       size_t maxCarry = 4096) {
  carry->append(chunk);
  size_t newline = carry->rfind('\n');
  if (newline != string::npos) {
    *carry = carry->substr(newline + 1);
  } else if (carry->size() > maxCarry) {
    *carry = carry->substr(carry->size() - maxCarry);
  }
}

}  // namespace et

#endif  // __ET_TMUX_CC_FILTER__
