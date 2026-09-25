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

TEST_CASE("filterTmuxCc finishes a mid-line %output remainder",
          "[TmuxCcFilter]") {
  const string rest =
      "OOD_1\\012FLOOD_2\n"
      "%extended-output %0 0 : more\n"
      "%layout-change @1 vis\n";
  TmuxCcFilterResult result = filterTmuxCc(rest, true);
  REQUIRE(result.kept ==
          "OOD_1\\012FLOOD_2\n"
          "%layout-change @1 vis\n");
  REQUIRE(result.dropped == string("%extended-output %0 0 : more\n").size());
  REQUIRE_FALSE(result.skipUntilNewline);
}

TEST_CASE("filterTmuxCc keeps an incomplete mid-line control payload",
          "[TmuxCcFilter]") {
  TmuxCcFilterResult result = filterTmuxCc(string(100, 'y'), true);
  REQUIRE(result.kept == string(100, 'y'));
  REQUIRE(result.dropped == 0);
  REQUIRE_FALSE(result.skipUntilNewline);
}

TEST_CASE("filterTmuxCc TTY incomplete still does not skip", "[TmuxCcFilter]") {
  TmuxCcFilterResult result = filterTmuxCc("yyyyyyyy", false);
  REQUIRE(result.kept.empty());
  REQUIRE_FALSE(result.skipUntilNewline);
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
  REQUIRE(tmuxCcContainsInterruptCommand("send -H -t %0 03\n"));
  REQUIRE(tmuxCcContainsInterruptCommand("send -Ht %0 03\n"));
  REQUIRE(tmuxCcContainsInterruptCommand("send -t %0 0x03\n"));
  REQUIRE(tmuxCcContainsInterruptCommand("send -t %0 0x3\n"));
  REQUIRE(tmuxCcContainsInterruptCommand("send -t %0 0x1a\n"));
  REQUIRE(tmuxCcContainsInterruptCommand(
      "send -t %0 0x3\nsend -t %0 0x3\nsend -t %0 0x3\n"));
  REQUIRE(tmuxCcContainsInterruptCommand("send -t %0 0x3\rsend -t %0 0x3\r"));
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
  REQUIRE_FALSE(tmuxCcContainsInterruptCommand("send-keys 3\n"));
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

TEST_CASE("tmuxCcInputRequestsInterrupt joins a split send-keys command",
          "[TmuxCcFilter][TmuxCc]") {
  REQUIRE_FALSE(tmuxCcContainsInterruptCommand("send-keys -t %0 -H "));
  REQUIRE(tmuxCcInputRequestsInterrupt("send-keys -t %0 -H ", "03\n"));
}

TEST_CASE("TmuxCcInjectionFilter passes shell output before control mode",
          "[TmuxCcFilter]") {
  TmuxCcInjectionFilter filter;
  const string prompt = "user@host:~$ ";
  REQUIRE(filter.apply(prompt) == prompt);
  REQUIRE(filter.apply("ls\r\nfile\r\n") == "ls\r\nfile\r\n");
}

TEST_CASE("TmuxCcInjectionFilter drops a journald wall inside tmux -CC",
          "[TmuxCcFilter]") {
  TmuxCcInjectionFilter filter;
  const string dcs = "\x1bP1000p";
  const string head = dcs + "%session-changed $0 wall\n";
  REQUIRE(filter.apply(head) == head);

  const string wall =
      "\r\n"
      "Broadcast message from systemd-journald@host "
      "(Wed 2026-09-23 23:04:10 UTC):\r\n"
      "\r\n"
      "journald-test[4556]: Test message at priority: emerg "
      "WALL_INJECT_856\r\n"
      "\r\n";
  REQUIRE(filter.apply(wall).empty());

  const string output = "%output %0 still-alive\n\x1b\\";
  REQUIRE(filter.apply(output) == output);
  REQUIRE(filter.apply("%begin 1 2 0\nAFTER_WALL\n%end 1 2 0\n") ==
          "%begin 1 2 0\nAFTER_WALL\n%end 1 2 0\n");
}

TEST_CASE("TmuxCcInjectionFilter reassembles a wall line split across reads",
          "[TmuxCcFilter]") {
  TmuxCcInjectionFilter filter;
  const string dcs = "\x1bP1000p";
  REQUIRE(filter.apply(dcs + "%sessions-changed\n") ==
          dcs + "%sessions-changed\n");
  REQUIRE(filter.apply("Broad").empty());
  REQUIRE(filter.apply("cast message from systemd-journald\r\n").empty());
  REQUIRE(filter.apply("%window-add @0\n") == "%window-add @0\n");
}

TEST_CASE("TmuxCcInjectionFilter ignores bare percent line without DCS",
          "[TmuxCcFilter]") {
  TmuxCcInjectionFilter filter;
  REQUIRE(filter.apply("%output %0 fake\n") == "%output %0 fake\n");
  REQUIRE(filter.apply("hello\n") == "hello\n");
}

TEST_CASE("TmuxCcInjectionFilter bare %sessions-changed does not enter mode",
          "[TmuxCcFilter]") {
  TmuxCcInjectionFilter filter;
  REQUIRE(filter.apply("%sessions-changed\n") == "%sessions-changed\n");
  const string shell = "user@host:~$ ls\r\nfile\r\n";
  REQUIRE(filter.apply(shell) == shell);
}

TEST_CASE(
    "TmuxCcInjectionFilter forwards shell output after ST ends control mode",
    "[TmuxCcFilter]") {
  TmuxCcInjectionFilter filter;
  const string dcs = "\x1bP1000p";
  REQUIRE(filter.apply(dcs + "%session-changed $0 wall\n") ==
          dcs + "%session-changed $0 wall\n");
  REQUIRE(filter.apply("\x1b\\") == "\x1b\\");
  const string shell = "user@host:~$ ls\r\nfile\r\n";
  REQUIRE(filter.apply(shell) == shell);
}

TEST_CASE("TmuxCcInjectionFilter clears control mode on %exit",
          "[TmuxCcFilter]") {
  TmuxCcInjectionFilter filter;
  const string dcs = "\x1bP1000p";
  REQUIRE(filter.apply(dcs + "%session-changed $0 wall\n") ==
          dcs + "%session-changed $0 wall\n");
  REQUIRE(filter.apply("%exit\n") == "%exit\n");
  const string shell = "user@host:~$ ls\r\nfile\r\n";
  REQUIRE(filter.apply(shell) == shell);
}

TEST_CASE(
    "TmuxCcInjectionFilter arms control mode when DCS arrives with a partial "
    "first line",
    "[TmuxCcFilter][InjectionFilter]") {
  TmuxCcInjectionFilter filter;
  const string dcs = "\x1bP1000p";
  // DCS plus an incomplete first notification (no newline yet) must be held
  // so control mode can arm when the line completes.
  REQUIRE(filter.apply(dcs + "%session-changed").empty());
  REQUIRE(filter.apply(" $0 wall\n") == dcs + "%session-changed $0 wall\n");

  const string wall =
      "Broadcast message from systemd-journald@host "
      "(Wed 2026-09-23 23:04:10 UTC):\r\n";
  REQUIRE(filter.apply(wall).empty());
  REQUIRE(filter.apply("%output %0 still-alive\n") ==
          "%output %0 still-alive\n");
}

TEST_CASE(
    "TmuxCcInjectionFilter exits control mode when ST is coalesced with shell "
    "text",
    "[TmuxCcFilter][InjectionFilter]") {
  TmuxCcInjectionFilter filter;
  const string dcs = "\x1bP1000p";
  REQUIRE(filter.apply(dcs + "%session-changed $0 wall\n") ==
          dcs + "%session-changed $0 wall\n");

  // tmux may write the DCS terminator stuck to the following shell prompt.
  const string prompt = "user@host$ ";
  REQUIRE(filter.apply(string("\x1b\\") + prompt) == string("\x1b\\") + prompt);
  REQUIRE(filter.apply("ls\n") == "ls\n");
}

TEST_CASE(
    "TmuxCcInjectionFilter does not glue a held wall fragment onto a following "
    "% notification",
    "[TmuxCcFilter][InjectionFilter]") {
  TmuxCcInjectionFilter filter;
  const string dcs = "\x1bP1000p";
  REQUIRE(filter.apply(dcs + "%session-changed $0 wall\n") ==
          dcs + "%session-changed $0 wall\n");

  // Short read left a wall prefix in pending_; the next read is a real
  // control notification. The wall prefix must be dropped, not concatenated.
  REQUIRE(filter.apply("Broad").empty());
  REQUIRE(filter.apply("%exit\n") == "%exit\n");
  const string shell = "user@host:~$ ls\r\nfile\r\n";
  REQUIRE(filter.apply(shell) == shell);

  TmuxCcInjectionFilter filter2;
  REQUIRE(filter2.apply(dcs + "%session-changed $0 wall\n") ==
          dcs + "%session-changed $0 wall\n");
  REQUIRE(filter2.apply("Broad").empty());
  REQUIRE(filter2.apply("%window-add @0\n") == "%window-add @0\n");
  REQUIRE(filter2.apply("still-alive\n").empty());
  REQUIRE(filter2.apply("%output %0 pane\n") == "%output %0 pane\n");
}

TEST_CASE("TmuxCcInjectionFilter detects ST after a held wall fragment",
          "[TmuxCcFilter][InjectionFilter]") {
  TmuxCcInjectionFilter filter;
  const string dcs = "\x1bP1000p";
  REQUIRE(filter.apply(dcs + "%session-changed $0 wall\n") ==
          dcs + "%session-changed $0 wall\n");

  REQUIRE(filter.apply("Broad").empty());
  REQUIRE(filter.apply("\x1b\\") == "\x1b\\");
  const string shell = "user@host:~$ ls\r\nfile\r\n";
  REQUIRE(filter.apply(shell) == shell);
}

TEST_CASE("TmuxCcInjectionFilter drops wall text after a bare DCS introducer",
          "[TmuxCcFilter][InjectionFilter]") {
  TmuxCcInjectionFilter filter;
  const string dcs = "\x1bP1000p";
  REQUIRE(filter.apply(dcs).empty());
  const string out = filter.apply("Broadcast message\r\n");
  REQUIRE(out.find("Broadcast") == string::npos);
  REQUIRE(out.find(dcs) != string::npos);

  REQUIRE(filter.apply("Broadcast message from systemd-journald\r\n").empty());
  REQUIRE(filter.apply("%output %0 still-alive\n") ==
          "%output %0 still-alive\n");
}

TEST_CASE("TmuxCcInjectionFilter does not arm on DCS bytes embedded mid-line",
          "[TmuxCcFilter][InjectionFilter]") {
  TmuxCcInjectionFilter filter;
  const string dcs = "\x1bP1000p";
  // Shell text that merely contains the DCS byte sequence must not latch
  // control mode or truncate the line to the introducer alone.
  const string embedded = string("echo ") + dcs + " mid\n";
  REQUIRE(filter.apply(embedded) == embedded);
  const string shell = "user@host:~$ ls\r\nfile\r\n";
  REQUIRE(filter.apply(shell) == shell);
}

TEST_CASE(
    "TmuxCcInjectionFilter clears mode when ST follows a held incomplete % "
    "line",
    "[TmuxCcFilter][InjectionFilter]") {
  TmuxCcInjectionFilter filter;
  const string dcs = "\x1bP1000p";
  REQUIRE(filter.apply(dcs + "%session-changed $0 wall\n") ==
          dcs + "%session-changed $0 wall\n");

  // Short read of "%exit" leaves pending_ starting with '%'. ST must still
  // clear the latch so post-control-mode shell is forwarded.
  REQUIRE(filter.apply("%ex").empty());
  REQUIRE(filter.apply("\x1b\\") == "\x1b\\");
  const string shell = "user@host:~$ ls\r\nfile\r\n";
  REQUIRE(filter.apply(shell) == shell);
}

TEST_CASE(
    "TmuxCcInjectionFilter ignores pre-DCS %begin when latching begin block",
    "[TmuxCcFilter][InjectionFilter]") {
  TmuxCcInjectionFilter filter;
  const string dcs = "\x1bP1000p";

  // Shell can echo a literal "%begin ..." line before tmux -CC starts. That
  // must not leave inBeginBlock_ set, or a later wall is forwarded as body.
  REQUIRE(filter.apply("%begin 1 2 0\n") == "%begin 1 2 0\n");
  REQUIRE(filter.apply(dcs + "%session-changed $0 wall\n") ==
          dcs + "%session-changed $0 wall\n");
  REQUIRE(filter.apply("Broadcast message\r\n").empty());

  // A real %begin…%end body after control mode is armed must still be kept.
  REQUIRE(filter.apply("%begin 1 2 0\nAFTER_WALL\n%end 1 2 0\n") ==
          "%begin 1 2 0\nAFTER_WALL\n%end 1 2 0\n");
}

TEST_CASE(
    "TmuxCcInjectionFilter clears control mode on wall line with trailing ST",
    "[TmuxCcFilter][InjectionFilter]") {
  TmuxCcInjectionFilter filter;
  const string dcs = "\x1bP1000p";
  REQUIRE(filter.apply(dcs + "%session-changed $0 wall\n") ==
          dcs + "%session-changed $0 wall\n");

  // Newline-terminated wall text with an embedded trailing ST is dropped by
  // filterCompletedLine without seeing a leading terminator; control mode
  // must still clear so the following shell line is forwarded.
  const string wallWithSt = "Broadcast\x1b\\\n";
  const string out = filter.apply(wallWithSt);
  REQUIRE(out.find("Broadcast") == string::npos);
  REQUIRE(filter.apply("prompt$ ls\n") == "prompt$ ls\n");
}
