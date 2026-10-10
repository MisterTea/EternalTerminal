#ifndef __ET_OSC133_HPP__
#define __ET_OSC133_HPP__

#include <climits>
#include <string>
#include <vector>

// --- OSC 133 semantic-prompt support -------------------------------------
// A shell with FinalTerm/iTerm2 shell integration brackets each command with
// OSC 133 marks: "C" (output starts) and "D;<exit>" (command done). When the
// remote prompt emits them, etctl's `run` can frame commands without injecting
// its own echo markers -- keeping the scrollback clean -- and read the exit
// code straight from D. This header holds the pure parsing (no I/O) so it can
// be unit-tested directly; the detection cache and pty probe live in the etctl
// main, which includes this. A ST- or BEL-terminated form is accepted (shells
// differ).
namespace et {

// The command marks. C = "output starts here", D = "command done" with an
// optional numeric exit code. Integrations vary the payload -- iTerm2's
// fish/xonsh append ";aid=<id>", C may carry ";<CR>" or ";cmdline_url=..." --
// so trailing annotations are skipped, and either a BEL (\a) or an ST (ESC \)
// terminates (shells differ). The remote host controls this text, so scan it
// linearly rather than with a backtracking regex. On success, *pos is the
// mark's first byte, *end the byte after its terminator, and *code the D
// mark's exit status (-1 when absent or for C marks).
inline bool osc133FindMark(const std::string& s, size_t from, char mark,
                           size_t* pos, size_t* end, int* code) {
  for (size_t i = s.find('\x1b', from); i != std::string::npos;
       i = s.find('\x1b', i + 1)) {
    if (s.size() - i < 7 || s.compare(i, 6, "\x1b]133;") != 0 ||
        s[i + 6] != mark) {
      continue;
    }
    size_t p = i + 7;
    int parsedCode = -1;
    if (mark == 'D' && p < s.size() && s[p] == ';') {
      size_t q = p + 1;
      int value = 0;
      bool overflow = false;
      while (q < s.size() && s[q] >= '0' && s[q] <= '9') {
        const int digit = s[q] - '0';
        if (value > (INT_MAX - digit) / 10) {
          overflow = true;
        } else if (!overflow) {
          value = value * 10 + digit;
        }
        ++q;
      }
      if (q > p + 1 && !overflow) {
        parsedCode = value;
      }
      p = q;
    }
    // One pass for the terminator: BEL, or ST (ESC \). Another ESC first
    // means this mark is still unterminated; skip to that ESC.
    for (size_t t = p; t < s.size(); ++t) {
      if (s[t] == '\x07' ||
          (s[t] == '\x1b' && t + 1 < s.size() && s[t + 1] == '\\')) {
        *pos = i;
        *end = t + (s[t] == '\x07' ? 1 : 2);
        *code = parsedCode;
        return true;
      }
      if (s[t] == '\x1b') {
        break;
      }
    }
  }
  return false;
}

// Return the start of an SGR ending at `end`, or `end` if absent.
inline size_t osc133SgrStart(const std::string& s, size_t begin, size_t end) {
  if (end <= begin || s[end - 1] != 'm') return end;
  size_t p = end - 1;
  while (p > begin && ((s[p - 1] >= '0' && s[p - 1] <= '9') || s[p - 1] == ';'))
    --p;
  if (p < begin + 2 || s[p - 1] != '[' || s[p - 2] != '\x1b') return end;
  return p - 2;
}

// Recognize exactly SGR* glyph SGR* space-pad CR (space? CR)*.
inline size_t osc133EolDanceStart(const std::string& s, size_t begin,
                                  size_t end) {
  size_t p = end;
  if (p <= begin || s[p - 1] != '\r') return end;
  do {
    --p;
    if (p >= begin + 2 && s[p - 1] == ' ' && s[p - 2] == '\r') --p;
  } while (p > begin && s[p - 1] == '\r');
  const size_t padEnd = p;
  while (p > begin && s[p - 1] == ' ') --p;
  if (p == padEnd) return end;
  size_t next;
  while ((next = osc133SgrStart(s, begin, p)) < p) p = next;
  if (p == begin || s[p - 1] == '\x1b' || s[p - 1] == '\r' ||
      s[p - 1] == '\n' || s[p - 1] == ' ')
    return end;
  --p;
  while ((next = osc133SgrStart(s, begin, p)) < p) p = next;
  return p;
}

// Return the end of an eligible prep sequence at `begin`, or `begin`.
inline size_t osc133PrepEnd(const std::string& s, size_t begin, size_t end,
                            bool leading) {
  if (begin >= end) return begin;
  if (leading && s[begin] == '\r') {
    size_t p = begin + 1;
    while (p < end && s[p] == '\r') ++p;
    return p;
  }
  if (s[begin] != '\x1b' || end - begin < 2) return begin;
  if (end - begin >= 8 && s.compare(begin, 7, "\x1b[?2004") == 0 &&
      (s[begin + 7] == 'h' || s[begin + 7] == 'l'))
    return begin + 8;
  if (leading && s[begin + 1] == '[') {
    size_t p = begin + 2;
    while (p < end && (s[p] == '0' || s[p] == ';')) ++p;
    if (p < end && s[p] == 'm') return p + 1;
    return begin;
  }
  if (s[begin + 1] != ']') return begin;
  size_t p = begin + 2;
  if (end - p >= 5 && s.compare(p, 5, "1337;") == 0)
    p += 5;
  else if (end - p >= 2 && s.compare(p, 2, "7;") == 0)
    p += 2;
  else if (end - p >= 5 && s.compare(p, 4, "133;") == 0 && s[p + 4] >= 'A' &&
           s[p + 4] <= 'D')
    p += 5;
  else if (leading && end - p >= 2 && s[p] >= '0' && s[p] <= '2' &&
           s[p + 1] == ';')
    p += 2;
  else
    return begin;
  while (p < end) {
    if (s[p] == '\x07') return p + 1;
    if (s[p] == '\x1b') {
      return p + 1 < end && s[p + 1] == '\\' ? p + 2 : begin;
    }
    ++p;
  }
  return begin;
}

// A command's real output is bracketed by C..D, but shells splice prompt-prep
// sequences into that range as they tear down and set up the prompt. Observed
// across the official iTerm2 integrations: bash appends OSC 1337 (host/cwd) and
// a bracketed-paste enable before D; zsh and fish add an end-of-line "mark"
// dance (inverse glyph, space-pad to the column, CR); fish and xonsh prepend an
// OSC-0 title and an SGR reset right after C. Strip those from BOTH ends of the
// body, stopping at the first byte of real output, so the result matches the
// echo-marker framing (pure output). Eligibility is deliberately conservative:
//   - OSC 1337/7/133 and bracketed-paste: prompt-owned, stripped from either
//     end.
//   - OSC 0/1/2 title, a *zero/empty* SGR reset, and leading CRs: stripped only
//     from the FRONT (a command may legitimately end with a title or a reset).
//   - the zsh/fish EOL-mark dance: stripped only from the BACK.
// A non-reset SGR (real color, e.g. "\e[31m") is never eligible, so colored
// command output is preserved verbatim.
inline void trimPromptPrep(std::string* body) {
  // Move indices rather than erasing, and visit each ESC once, so stripping
  // many adjacent marks stays linear.
  size_t begin = 0, end = body->size();
  size_t next;
  while ((next = osc133PrepEnd(*body, begin, end, true)) > begin) begin = next;
  std::vector<size_t> escs;
  for (size_t i = body->find('\x1b', begin); i < end;
       i = body->find('\x1b', i + 1)) {
    escs.push_back(i);
  }
  size_t cachedEsc = std::string::npos, cachedPrepEnd = 0;
  while (end > begin) {
    while (!escs.empty() && escs.back() >= end) escs.pop_back();
    size_t esc = escs.empty() ? std::string::npos : escs.back();
    // A trailing ST's ESC does not start the sequence.
    if (esc != std::string::npos && esc + 2 == end &&
        (*body)[end - 1] == '\\') {
      esc = escs.size() >= 2 ? escs[escs.size() - 2] : std::string::npos;
    }
    if (esc != std::string::npos) {
      // A sequence's end does not change as `end` shrinks, only whether it
      // still reaches `end`.
      if (esc != cachedEsc) {
        cachedEsc = esc;
        cachedPrepEnd = osc133PrepEnd(*body, esc, end, false);
      }
      if (cachedPrepEnd == end) {
        end = esc;
        continue;
      }
    }
    next = osc133EolDanceStart(*body, begin, end);
    if (next == end) break;
    end = next;
  }
  *body = body->substr(begin, end - begin);
}

// True if `acc` contains a terminated mark of the given kind.
inline bool osc133ContainsMark(const std::string& acc, char mark) {
  size_t pos = 0, end = 0;
  int code = 0;
  return osc133FindMark(acc, 0, mark, &pos, &end, &code);
}

// Pull a command's output and exit code out of an OSC-133-framed stream. `acc`
// is the raw bytes captured since just before the command was typed. Returns
// true once the command's done-mark has arrived.
//
// Anchor on the first C, not on the first D. Everything before that C belongs
// to the prompt that was already on screen when capture started, and a shell
// that is still settling puts a D there: precmd emits the *previous* line's
// done-mark, so a line that was just Ctrl-C'd contributes a bare "D;130" with
// no C of its own. Anchoring on the first D would latch onto that orphan --
// and because `acc` only ever grows at the end, the first D never changes, so
// the read could never complete no matter how much valid output arrived after
// it. That is a permanent wedge, not a slow read; skipping orphan Ds is what
// keeps a stale byte from poisoning the rest of the session.
//
// Within the command's own range the D to use is still the FIRST one (a chatty
// integration such as fish emits another at the next prompt), and the C is the
// LAST one before it: fish and xonsh emit a semantic C annotation
// (";cmdline_url", ";aid") ahead of the real output-start C, and tcsh can emit
// stray Cs, so the closest C to the output is the right boundary. `body` is the
// bytes between, with prompt-prep trimmed from both ends; `code` is D's exit
// status, or -1 if D carried none.
inline bool extractOsc133(const std::string& acc, std::string* body,
                          int* code) {
  size_t cPos = 0, cEnd = 0;
  int ignored = 0;
  if (!osc133FindMark(acc, 0, 'C', &cPos, &cEnd, &ignored)) {
    return false;  // not started yet
  }
  const size_t firstC = cPos;

  // First D at or after the first C: this command's done-mark.
  size_t dPos = 0, dEnd = 0;
  if (!osc133FindMark(acc, firstC, 'D', &dPos, &dEnd, code)) {
    return false;  // not finished yet
  }

  // Last C before that D; the body starts after its terminator.
  size_t bodyStart = cEnd;
  while (osc133FindMark(acc, cEnd, 'C', &cPos, &cEnd, &ignored) &&
         cPos < dPos) {
    bodyStart = cEnd;
  }

  *body = acc.substr(bodyStart, dPos - bodyStart);
  trimPromptPrep(body);
  return true;
}

}  // namespace et

#endif  // __ET_OSC133_HPP__
