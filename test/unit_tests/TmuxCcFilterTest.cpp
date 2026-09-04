#include "TestHeaders.hpp"
#include "TmuxCcFilter.hpp"

using namespace et;

TEST_CASE("filterTmuxCc drops TTY floods", "[TmuxCcFilter]") {
  TmuxCcFilterResult result = filterTmuxCc(string(100, 'A') + "\nmore");
  REQUIRE(result.kept.empty());
  REQUIRE(result.dropped == 100 + 1 + 4);
  REQUIRE_FALSE(result.skipUntilNewline);
}

TEST_CASE("filterTmuxCc drops %output and keeps layout", "[TmuxCcFilter]") {
  const string stream =
      "%output %0 yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy\n"
      "%layout-change @1 layout visible flags\n"
      "%output %0 more-pane-bytes\n"
      "%session-changed $1 mysession\n";
  TmuxCcFilterResult result = filterTmuxCc(stream);
  REQUIRE(result.kept ==
          "%layout-change @1 layout visible flags\n"
          "%session-changed $1 mysession\n");
  REQUIRE(result.dropped == stream.size() - result.kept.size());
  REQUIRE_FALSE(result.skipUntilNewline);
}

TEST_CASE("filterTmuxCc keeps begin/end blocks", "[TmuxCcFilter]") {
  const string stream =
      "%output %0 flood\n"
      "%begin 1 2\n"
      "0: ksh* (1 panes)\n"
      "%end 1 2\n";
  TmuxCcFilterResult result = filterTmuxCc(stream);
  REQUIRE(result.kept ==
          "%begin 1 2\n"
          "0: ksh* (1 panes)\n"
          "%end 1 2\n");
}

TEST_CASE("filterTmuxCc keeps an incomplete control line", "[TmuxCcFilter]") {
  TmuxCcFilterResult result =
      filterTmuxCc("%output %0 x\n%layout-change @1 foo");
  REQUIRE(result.kept == "%layout-change @1 foo");
  REQUIRE_FALSE(result.skipUntilNewline);
}

TEST_CASE("filterTmuxCc drops an incomplete %output line", "[TmuxCcFilter]") {
  TmuxCcFilterResult result =
      filterTmuxCc("%output %0 lots-of-data-no-newline");
  REQUIRE(result.kept.empty());
  REQUIRE(result.skipUntilNewline);
}

TEST_CASE("filterTmuxCc drops incomplete TTY", "[TmuxCcFilter]") {
  TmuxCcFilterResult result = filterTmuxCc("yyyyyyyy");
  REQUIRE(result.kept.empty());
  REQUIRE_FALSE(result.skipUntilNewline);
}

TEST_CASE("filterTmuxCc keeps %extended-output only if not output",
          "[TmuxCcFilter]") {
  TmuxCcFilterResult dropped =
      filterTmuxCc("%extended-output %0 12 : abc\\012def\n%window-add @2\n");
  REQUIRE(dropped.kept == "%window-add @2\n");
}

// A GUI attached with `tmux -CC` (e.g. iTerm2) sends keystrokes as
// `send-keys` control-mode commands rather than raw bytes, so Ctrl+C never
// appears as byte 0x03 on the wire. These cover the command forms that must
// still be recognized as an interrupt request.
TEST_CASE("tmuxCcContainsInterruptCommand recognizes hex-encoded Ctrl+C/Z/\\",
          "[TmuxCcFilter][TmuxCc]") {
  REQUIRE(tmuxCcContainsInterruptCommand("send-keys -H 3\n"));
  REQUIRE(tmuxCcContainsInterruptCommand("send-keys -H 03\n"));
  REQUIRE(tmuxCcContainsInterruptCommand("send-keys -H 1a\n"));
  REQUIRE(tmuxCcContainsInterruptCommand("send-keys -H 1A\n"));
  REQUIRE(tmuxCcContainsInterruptCommand("send-keys -H 1c\n"));
  REQUIRE(tmuxCcContainsInterruptCommand("send-keys -t %1 -H 3\n"));
}

TEST_CASE("tmuxCcContainsInterruptCommand recognizes key names",
          "[TmuxCcFilter][TmuxCc]") {
  REQUIRE(tmuxCcContainsInterruptCommand("send-keys C-c\n"));
  REQUIRE(tmuxCcContainsInterruptCommand("send-keys -t %0 C-c\n"));
  REQUIRE(tmuxCcContainsInterruptCommand("send C-c\n"));
  REQUIRE(tmuxCcContainsInterruptCommand("send-keys ^C\n"));
  REQUIRE(tmuxCcContainsInterruptCommand("send-keys C-z\n"));
  REQUIRE(tmuxCcContainsInterruptCommand("send-keys C-\\\n"));
}

TEST_CASE("tmuxCcContainsInterruptCommand ignores non-interrupt input",
          "[TmuxCcFilter][TmuxCc]") {
  REQUIRE_FALSE(tmuxCcContainsInterruptCommand(""));
  REQUIRE_FALSE(tmuxCcContainsInterruptCommand("send-keys -H 41\n"));
  REQUIRE_FALSE(tmuxCcContainsInterruptCommand("send-keys hello Enter\n"));
  REQUIRE_FALSE(tmuxCcContainsInterruptCommand("send-keys -l C-c\n"));
  REQUIRE_FALSE(tmuxCcContainsInterruptCommand("list-windows\n"));
  // A pane echoing the literal text is server->client output, not a client
  // command; the leading token is %output, not send-keys/send.
  REQUIRE_FALSE(tmuxCcContainsInterruptCommand("%output %0 send-keys -H 3\n"));
}

TEST_CASE("tmuxCcContainsInterruptCommand scans every line in a chunk",
          "[TmuxCcFilter][TmuxCc]") {
  REQUIRE(tmuxCcContainsInterruptCommand(
      "list-windows\nsend-keys -t %2 -H 3\nrefresh-client\n"));
}
