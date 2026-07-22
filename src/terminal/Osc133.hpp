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
// true once the command's done-mark has arrived. Anchor on the FIRST D: it is
// this command's, and a chatty integration (fish) emits another D at the next
// prompt that must not be mistaken for it. Then take the LAST C before that D:
// fish and xonsh emit a semantic C annotation (";cmdline_url", ";aid") before
// the real output-start C, and tcsh can emit stray Cs, so the closest C to the
// output is the right boundary. `body` is the bytes between, with prompt-prep
// trimmed from both ends; `code` is D's exit status, or -1 if D carried none.
inline bool extractOsc133(const std::string& acc, std::string* body,
                          int* code) {
  std::smatch dm;
  if (!std::regex_search(acc, dm, kOsc133D)) return false;  // not finished yet
  *code = dm[2].matched ? atoi(dm[2].str().c_str()) : -1;
  const std::string beforeD = acc.substr(0, (size_t)dm.position(0));
  size_t bodyStart = std::string::npos;
  for (auto it = std::sregex_iterator(beforeD.begin(), beforeD.end(), kOsc133C);
       it != std::sregex_iterator(); ++it) {
    bodyStart = (size_t)it->position(0) + it->length(0);  // last C before D
  }
  if (bodyStart == std::string::npos) return false;  // D seen, but no C yet
  *body = beforeD.substr(bodyStart);
  trimPromptPrep(body);
  return true;
}

}  // namespace et

#endif  // __ET_OSC133_HPP__
