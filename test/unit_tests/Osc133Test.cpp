// Unit tests for the pure OSC-133 parsing in Osc133.hpp: pulling a command's
// output and exit code out of an integrated prompt's C..D marks, and trimming
// the prompt-prep sequences shells splice into that range. The byte fixtures
// are distilled from real captures of the official iTerm2 shell integrations
// running under a pty (bash, zsh, tcsh, fish, xonsh), so a regression here maps
// to a concrete stream one of those shells actually emits.
#include "Osc133.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {

// OSC-133 marks, in the terminator + payload forms the various shells use.
const string C_BEL = "\x1b]133;C\x07";
const string C_CR_BEL = "\x1b]133;C;\r\x07";  // bash/tcsh form
string cAid(const string& id) {               // fish/xonsh form
  return "\x1b]133;C;aid=" + id + "\r\x07";
}
string dBel(int code) { return "\x1b]133;D;" + std::to_string(code) + "\x07"; }
string dSt(int code) {
  return "\x1b]133;D;" + std::to_string(code) + "\x1b\\";  // ST-terminated
}
string dAid(int code, const string& id) {  // xonsh: code then aid
  return "\x1b]133;D;" + std::to_string(code) + ";aid=" + id + "\x07";
}

// The prompt-prep pieces each integration splices around the output.
const string kBashTail =  // iTerm2 bash: OSC 1337 context + bracketed-paste
    "\x1b]1337;RemoteHost=me@host\x07\x1b]1337;CurrentDir=/tmp\x07\x1b[?2004h";
string eolDance(const string& glyph) {  // zsh/fish end-of-line mark
  return "\x1b[1m\x1b[7m" + glyph + "\x1b[27m\x1b[1m\x1b[0m" + string(40, ' ') +
         "\r \r";
}
string fishLead(
    const string& cmdline) {  // fish/xonsh: OSC-0 title + reset + CR
  return "\x1b]0;" + cmdline + " /tmp\x1b\\\x1b[m\r";
}

}  // namespace

TEST_CASE("extractOsc133 basic framing and exit codes", "[Osc133]") {
  string body;
  int code = -999;

  SECTION("BEL-terminated C and D") {
    REQUIRE(extractOsc133("in" + C_BEL + "HELLO\r\n" + dBel(0), &body, &code));
    REQUIRE(body == "HELLO\r\n");
    REQUIRE(code == 0);
  }

  SECTION("multi-digit exit code") {
    REQUIRE(extractOsc133(C_BEL + "boom\r\n" + dBel(137), &body, &code));
    REQUIRE(code == 137);
  }

  SECTION("ST-terminated D") {
    REQUIRE(extractOsc133(C_BEL + "out\r\n" + dSt(2), &body, &code));
    REQUIRE(body == "out\r\n");
    REQUIRE(code == 2);
  }

  SECTION("D with no numeric status yields -1") {
    REQUIRE(extractOsc133(C_BEL + "x\r\n\x1b]133;D\x07", &body, &code));
    REQUIRE(body == "x\r\n");
    REQUIRE(code == -1);
  }

  SECTION("no D yet: command still running") {
    REQUIRE_FALSE(extractOsc133(C_BEL + "partial", &body, &code));
  }

  SECTION("no C before D: not framed") {
    REQUIRE_FALSE(extractOsc133("noise\x1b]133;D;0\x07", &body, &code));
  }
}

TEST_CASE("extractOsc133 cleans each shell's prompt-prep", "[Osc133]") {
  string body;
  int code = -999;

  SECTION("bash: trailing OSC 1337 + bracketed-paste") {
    REQUIRE(extractOsc133(C_CR_BEL + "hello\r\n" + kBashTail + dBel(0), &body,
                          &code));
    REQUIRE(body == "hello\r\n");
    REQUIRE(code == 0);
  }

  SECTION("bash: no-newline output, trailing prep") {
    REQUIRE(
        extractOsc133(C_CR_BEL + "foo" + kBashTail + dBel(0), &body, &code));
    REQUIRE(body == "foo");
  }

  SECTION("zsh: trailing EOL-mark dance ('%')") {
    REQUIRE(extractOsc133(C_BEL + "hi\r\n" + eolDance("%") + dBel(0), &body,
                          &code));
    REQUIRE(body == "hi\r\n");
  }

  SECTION("zsh: customized PROMPT_EOL_MARK glyph") {
    REQUIRE(extractOsc133(C_BEL + "hi\r\n" + eolDance("@") + dBel(0), &body,
                          &code));
    REQUIRE(body == "hi\r\n");
  }

  SECTION("tcsh: clean C;<CR> frame, no prep") {
    REQUIRE(extractOsc133(C_CR_BEL + "hello\r\n" + dBel(0), &body, &code));
    REQUIRE(body == "hello\r\n");
  }

  SECTION("fish: leading title + reset, double C, exit 7") {
    // fish emits a cmdline-annotation C, then the output-start C; the last C
    // before D is the right boundary. Leading OSC-0 title + SGR reset + CR go.
    const string acc = cAid("a1") + fishLead("sh -c 'exit 7'") + dSt(7);
    REQUIRE(extractOsc133(acc, &body, &code));
    REQUIRE(body == "");
    REQUIRE(code == 7);
  }

  SECTION("fish: leading prep with real output") {
    const string acc =
        cAid("a2") + fishLead("echo hi") + "hi\r\n" + eolDance("%") + dSt(0);
    REQUIRE(extractOsc133(acc, &body, &code));
    REQUIRE(body == "hi\r\n");
  }

  SECTION("xonsh: aid on both C and D, leading title") {
    const string acc =
        cAid("e5") + fishLead("echo hi") + "hi\r\n" + dAid(0, "e5");
    REQUIRE(extractOsc133(acc, &body, &code));
    REQUIRE(body == "hi\r\n");
    REQUIRE(code == 0);
  }

  SECTION("fish: a second D at the next prompt is not used") {
    // First D wins; the trailing prompt-cycle D must be ignored.
    const string acc =
        C_CR_BEL + "out\r\n" + dBel(0) + "\x1b]133;A\x07\x1b]133;D;0\x07";
    REQUIRE(extractOsc133(acc, &body, &code));
    REQUIRE(body == "out\r\n");
    REQUIRE(code == 0);
  }
}

TEST_CASE("extractOsc133 preserves real output that looks prep-like",
          "[Osc133]") {
  string body;
  int code = -999;

  SECTION("output ending in '%'") {
    REQUIRE(extractOsc133(C_BEL + "done 50%" + eolDance("%") + dBel(0), &body,
                          &code));
    REQUIRE(body == "done 50%");
  }

  SECTION("output ending in '#' then newline") {
    REQUIRE(extractOsc133(C_BEL + "C#\r\n" + eolDance("%") + dBel(0), &body,
                          &code));
    REQUIRE(body == "C#\r\n");
  }

  SECTION("output ending in spaces but no CR") {
    REQUIRE(
        extractOsc133(C_BEL + "a%   " + eolDance("%") + dBel(0), &body, &code));
    REQUIRE(body == "a%   ");
  }

  SECTION("colored output keeps its non-reset SGR") {
    REQUIRE(
        extractOsc133(C_BEL + "\x1b[31mred\x1b[0m" + dBel(0), &body, &code));
    REQUIRE(body == "\x1b[31mred\x1b[0m");
  }

  SECTION("a title emitted at end of output is real output") {
    REQUIRE(extractOsc133(C_BEL + "data\x1b]0;t\x07" + dBel(0), &body, &code));
    REQUIRE(body == "data\x1b]0;t\x07");
  }

  SECTION("no prep present: body untouched") {
    REQUIRE(extractOsc133(C_BEL + "plain\r\n" + dBel(0), &body, &code));
    REQUIRE(body == "plain\r\n");
  }
}

// Regression: a capture that opens on prompt noise from the *previous* command.
// `run` snapshots its cursor at what it believes is an idle prompt, but a byte
// still in flight lands on the far side of it. The one that matters is the D
// precmd emits for the previous line -- a line that was just Ctrl-C'd leaves a
// bare "D;130" with no C of its own. Anchoring on the first D latched onto that
// orphan, and since `acc` only grows at the end, no later output could dislodge
// it: the read never completed, the command's output was discarded, and the
// session looked wedged. See bug-reports/etctl-run-false-parked-detection.md.
TEST_CASE("extractOsc133 ignores orphan done-marks from the previous prompt",
          "[Osc133]") {
  string body;
  int code = -999;
  const string A_BEL = "\x1b]133;A\x07";
  const string B_BEL = "\x1b]133;B\x07";
  // What zsh actually emits after a Ctrl-C at a continuation prompt: the
  // aborted line's D, then a fresh prompt, then our command.
  const string orphan = dBel(130) + A_BEL + "[15:24:43] ~ " + B_BEL;

  SECTION("orphan D before the command is skipped") {
    REQUIRE(extractOsc133(orphan + C_BEL + "RECOVERED\r\n" + dBel(0), &body,
                          &code));
    REQUIRE(body == "RECOVERED\r\n");
    REQUIRE(code == 0);  // ours, not the interrupted line's 130
  }

  SECTION("several orphan Ds are skipped") {
    REQUIRE(extractOsc133(orphan + orphan + C_BEL + "out\r\n" + dBel(3), &body,
                          &code));
    REQUIRE(body == "out\r\n");
    REQUIRE(code == 3);
  }

  SECTION("an orphan D still leaves the next command's D authoritative") {
    // A chatty integration emits another D at the following prompt; the first D
    // *after our C* is still the one that ends our output.
    REQUIRE(extractOsc133(
        orphan + C_BEL + "mine\r\n" + dBel(7) + A_BEL + B_BEL + dBel(0), &body,
        &code));
    REQUIRE(body == "mine\r\n");
    REQUIRE(code == 7);
  }

  SECTION("orphan D alone is not a completed command") {
    REQUIRE_FALSE(extractOsc133(orphan, &body, &code));
  }

  SECTION("orphan D then a C but no D yet: still running") {
    REQUIRE_FALSE(extractOsc133(orphan + C_BEL + "partial", &body, &code));
  }

  SECTION("fish's annotation C before the real C, behind an orphan D") {
    REQUIRE(extractOsc133(orphan + cAid("1") + fishLead("echo hi") + C_BEL +
                              "hi\r\n" + dAid(0, "1"),
                          &body, &code));
    REQUIRE(body == "hi\r\n");
    REQUIRE(code == 0);
  }
}
