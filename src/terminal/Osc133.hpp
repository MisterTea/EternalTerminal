#ifndef __ET_OSC133_HPP__
#define __ET_OSC133_HPP__

#include <cstdlib>
#include <regex>
#include <string>

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
// so [^\x07\x1b]* absorbs whatever trails the essential part, and either a BEL
// (\a) or an ST (ESC \) terminates (shells differ). kOsc133D's group 1 is the
// exit code when present.
inline const std::regex kOsc133C("\x1b\\]133;C[^\x07\x1b]*(\x07|\x1b\\\\)");
inline const std::regex kOsc133D(
    "\x1b\\]133;D(;([0-9]+))?[^\x07\x1b]*(\x07|\x1b\\\\)");

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
  static const std::regex lead[] = {
      std::regex("^\x1b\\]1337;[^\x07\x1b]*(\x07|\x1b\\\\)"),
      std::regex("^\x1b\\]7;[^\x07\x1b]*(\x07|\x1b\\\\)"),
      std::regex("^\x1b\\]133;[ABCD][^\x07\x1b]*(\x07|\x1b\\\\)"),
      std::regex("^\x1b\\[\\?2004[hl]"),
      std::regex("^\x1b\\][012];[^\x07\x1b]*(\x07|\x1b\\\\)"),  // window title
      std::regex(
          "^\x1b\\[0*(;0*)*m"),  // SGR reset (empty/all-zero params only)
      std::regex("^\r+"),
  };
  static const std::regex tail[] = {
      std::regex("\x1b\\]1337;[^\x07\x1b]*(\x07|\x1b\\\\)$"),
      std::regex("\x1b\\]7;[^\x07\x1b]*(\x07|\x1b\\\\)$"),
      std::regex("\x1b\\]133;[ABCD][^\x07\x1b]*(\x07|\x1b\\\\)$"),
      std::regex("\x1b\\[\\?2004[hl]$"),
      // zsh/fish EOL-mark dance: SGR* glyph SGR* space-pad CR (space CR)*.
      std::regex(
          "(\x1b\\[[0-9;]*m)*[^\x1b\r\n ](\x1b\\[[0-9;]*m)* +\r( ?\r)*$"),
  };
  bool changed = true;
  while (changed) {
    changed = false;
    for (const std::regex& re : lead) {
      std::string t = std::regex_replace(*body, re, "");
      if (t.size() != body->size()) {
        *body = std::move(t);
        changed = true;
      }
    }
    for (const std::regex& re : tail) {
      std::string t = std::regex_replace(*body, re, "");
      if (t.size() != body->size()) {
        *body = std::move(t);
        changed = true;
      }
    }
  }
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
  const std::sregex_iterator kEnd;
  std::smatch cm;
  if (!std::regex_search(acc, cm, kOsc133C)) return false;  // not started yet
  const size_t firstC = (size_t)cm.position(0);

  // First D at or after the first C: this command's done-mark.
  auto dIt =
      std::sregex_iterator(acc.begin() + (long)firstC, acc.end(), kOsc133D);
  if (dIt == kEnd) return false;  // not finished yet
  const std::smatch& dm = *dIt;
  const size_t dPos = firstC + (size_t)dm.position(0);

  // Last C before that D.
  size_t bodyStart = std::string::npos;
  for (auto it = std::sregex_iterator(acc.begin(), acc.begin() + (long)dPos,
                                      kOsc133C);
       it != kEnd; ++it) {
    bodyStart = (size_t)it->position(0) + it->length(0);
  }
  if (bodyStart == std::string::npos)
    return false;  // unreachable: firstC < dPos

  *code = dm[2].matched ? atoi(dm[2].str().c_str()) : -1;
  *body = acc.substr(bodyStart, dPos - bodyStart);
  trimPromptPrep(body);
  return true;
}

}  // namespace et

#endif  // __ET_OSC133_HPP__
