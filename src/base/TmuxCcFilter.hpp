#ifndef __ET_TMUX_CC_FILTER__
#define __ET_TMUX_CC_FILTER__

#include <cctype>

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

/**
 * @brief Drops tty bytes injected into a tmux -CC stream.
 *
 * journald walls emerg log lines onto every utmp tty. etterminal registers
 * its pty, so that broadcast is written onto the same slave tmux -CC uses
 * for the control protocol. A line in that stream that does not start with
 * '%' makes iTerm2 tear the session down. Response bodies inside
 * `%begin`…`%end` / `%error` are kept. Bytes from before control mode starts
 * pass through, so a normal shell is unchanged.
 */
class TmuxCcInjectionFilter {
 public:
  string apply(const string& chunk) {
    // A short read may leave a journald wall prefix in pending_ while the
    // next read starts a real tmux notification or ST. Drop the wall hold
    // so it is not glued onto "%exit" / "%window-add" / ST.
    if (inControlMode_ && isHeldWallFragment(pending_) &&
        chunkOpensControl(chunk)) {
      pending_.clear();
    }
    pending_.append(chunk);
    string out;
    size_t lineStart = 0;
    for (size_t i = 0; i < pending_.size(); ++i) {
      if (pending_[i] != '\n') {
        continue;
      }
      const string raw = pending_.substr(lineStart, i + 1 - lineStart);
      out.append(filterCompletedLine(raw));
      lineStart = i + 1;
    }
    if (lineStart > 0) {
      pending_.erase(0, lineStart);
    }
    // ST may follow a held wall fragment in the same pending_ buffer
    // (or arrive as its own write after the wall prefix was cleared above).
    if (inControlMode_ && !pending_.empty()) {
      if (!(pending_.size() >= kStLen &&
            pending_.compare(0, kStLen, kSt) == 0)) {
        const size_t stPos = pending_.find(kSt);
        // Drop a held wall fragment or incomplete '%' notification that
        // precedes ST so the terminator can clear the control-mode latch.
        if (stPos != string::npos && stPos > 0 &&
            (isHeldWallFragment(pending_.substr(0, stPos)) ||
             pending_[0] == '%')) {
          pending_.erase(0, stPos);
        }
      }
      if (pending_.size() >= kStLen && pending_.compare(0, kStLen, kSt) == 0) {
        // tmux writes the DCS terminator as its own write, with no trailing
        // newline. It may also be stuck to the following shell prompt in one
        // read. Strip a leading ST, exit control mode, then fall through so
        // any coalesced shell text is forwarded.
        out.append(pending_, 0, kStLen);
        pending_.erase(0, kStLen);
        inControlMode_ = false;
        inBeginBlock_ = false;
      }
    }
    // A shell prompt has no trailing newline. Hold bytes only once control
    // mode has started (so a split wall line can be dropped) or when the
    // tail might still grow into / continue a tmux DCS introducer.
    if (!inControlMode_ && !pending_.empty() && !isDcsPrefix(pending_)) {
      out.append(pending_);
      pending_.clear();
    }
    return out;
  }

 private:
  static constexpr char kDcs[] = "\x1bP1000p";
  static constexpr size_t kDcsLen = sizeof(kDcs) - 1;
  static constexpr char kSt[] = "\x1b\\";
  static constexpr size_t kStLen = sizeof(kSt) - 1;

  // True while @p text is a proper prefix of the DCS introducer, or already
  // begins with the full introducer (incomplete first notification line).
  static bool isDcsPrefix(const string& text) {
    if (text.size() <= kDcsLen) {
      return string(kDcs, kDcsLen).compare(0, text.size(), text) == 0;
    }
    return text.compare(0, kDcsLen, kDcs, kDcsLen) == 0;
  }

  static bool isStPrefix(const string& text) {
    if (text.empty() || text.size() > kStLen) {
      return false;
    }
    return string(kSt, kStLen).compare(0, text.size(), text) == 0;
  }

  // Incomplete pending bytes that are neither a control notification nor a
  // DCS/ST prefix — typically a journald wall line split across reads.
  static bool isHeldWallFragment(const string& text) {
    if (text.empty() || text[0] == '%' || isDcsPrefix(text) ||
        isStPrefix(text)) {
      return false;
    }
    return true;
  }

  static bool chunkOpensControl(const string& chunk) {
    if (chunk.empty()) {
      return false;
    }
    if (chunk[0] == '%') {
      return true;
    }
    const size_t stCheck = chunk.size() < kStLen ? chunk.size() : kStLen;
    if (isStPrefix(chunk.substr(0, stCheck))) {
      return true;
    }
    return isDcsPrefix(chunk);
  }

  static bool isTmuxNotification(const string& token) {
    return token.size() > 1 && token[0] == '%' &&
           std::isalpha(static_cast<unsigned char>(token[1]));
  }

  // Returns the bytes of @p rawLine to forward. A line that arms control mode
  // via DCS but then continues with wall text forwards only the DCS bytes.
  string filterCompletedLine(const string& rawLine) {
    string line = rawLine;
    if (!line.empty() && line.back() == '\n') {
      line.pop_back();
    }
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    const bool wasInControlMode = inControlMode_;
    // Arm only on a leading DCS introducer. Embedded \x1bP1000p in shell
    // output must not latch control mode or truncate the line.
    const bool sawDcs = line.compare(0, kDcsLen, kDcs) == 0;
    string body = line;
    if (body.compare(0, kDcsLen, kDcs) == 0) {
      body.erase(0, kDcsLen);
    }
    // ST may arrive alone or stuck to following shell text in one write.
    const bool sawTerminator = body.compare(0, kStLen, kSt) == 0;
    if (sawTerminator) {
      body.erase(0, kStLen);
    }
    const string token = tmuxCcFirstToken(body);
    const bool percentCommand = isTmuxNotification(token);

    if (sawDcs) {
      inControlMode_ = true;
    }
    // Only latch %begin once control mode is active (or this line itself
    // opens it via DCS). A pre-DCS shell echo of "%begin ..." must not leave
    // inBeginBlock_ set, or later wall text is forwarded as response body.
    if (percentCommand && token == "%begin" && (wasInControlMode || sawDcs)) {
      inBeginBlock_ = true;
    }

    // Do not treat bare sawDcs as keep-all: DCS + wall must forward only DCS.
    // Empty body after DCS (bare introducer line) is kept; a lone newline in
    // control mode is still dropped like other non-% text.
    const bool keepBody = percentCommand || sawTerminator || inBeginBlock_ ||
                          (sawDcs && body.empty());
    const bool keepAll = (!wasInControlMode && !sawDcs) || keepBody;

    if (percentCommand && (token == "%end" || token == "%error")) {
      inBeginBlock_ = false;
    }
    // Control mode ends on the DCS string terminator or an explicit %exit.
    // Clear the latch so ordinary shell output after the session is forwarded.
    if (sawTerminator || (percentCommand && token == "%exit")) {
      inControlMode_ = false;
      inBeginBlock_ = false;
    }

    if (keepAll) {
      return rawLine;
    }
    if (sawDcs) {
      return string(kDcs, kDcsLen);
    }
    // A completed wall line may carry a trailing ST (e.g. "Broadcast\x1b\\\n").
    // Leading-ST detection above misses that; the pending_ ST scan never runs
    // for a newline-terminated line already consumed here. Strip the wall,
    // forward ST, and exit control mode so post-control shell is kept.
    if (wasInControlMode && inControlMode_) {
      const size_t stPos = body.find(kSt);
      if (stPos != string::npos) {
        inControlMode_ = false;
        inBeginBlock_ = false;
        return string(kSt, kStLen);
      }
    }
    return "";
  }

  bool inControlMode_ = false;
  bool inBeginBlock_ = false;
  string pending_;
};

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
