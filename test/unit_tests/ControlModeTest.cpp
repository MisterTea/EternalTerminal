#include "ControlCommands.hpp"
#include "ControlMode.hpp"
#include "HtmTestHelpers.hpp"
#include "MultiplexerState.hpp"
#include "PaneScreen.hpp"
#include "TestHeaders.hpp"

using namespace et;
using namespace et::htmtest;

TEST_CASE("controlOctalEscape encodes control bytes and backslash",
          "[Htm][ControlMode]") {
  REQUIRE(controlOctalEscape("ab") == "ab");
  REQUIRE(controlOctalEscape("a\nb") == "a\\012b");
  REQUIRE(controlOctalEscape("\\") == "\\134");
  REQUIRE(controlOctalEscape(string(1, '\0') + "x") == "\\000x");
}

TEST_CASE("controlSplitArgs handles quotes", "[Htm][ControlMode]") {
  auto args =
      controlSplitArgs("list-windows -F '#{window_id} #{window_layout}'");
  REQUIRE(args.size() == 3);
  REQUIRE(args[0] == "list-windows");
  REQUIRE(args[1] == "-F");
  REQUIRE(args[2] == "#{window_id} #{window_layout}");
}

TEST_CASE("splitControlCommandList splits on unquoted semicolons",
          "[Htm][ControlMode]") {
  auto cmds = splitControlCommandList(
      "show -v -q -t $1 @iterm2_id; refresh-client -C 80,25; list-windows -F "
      "\"#{window_id}\t#{?window_active,1,0}\"");
  REQUIRE(cmds.size() == 3);
  REQUIRE(cmds[0] == "show -v -q -t $1 @iterm2_id");
  REQUIRE(cmds[1] == "refresh-client -C 80,25");
  REQUIRE(cmds[2].find("list-windows") == 0);
  REQUIRE(cmds[2].find("#{?window_active,1,0}") != string::npos);
  auto one = splitControlCommandList("list-windows -F \"a;b\"");
  REQUIRE(one.size() == 1);
}

TEST_CASE("parseControlCommand extracts flags", "[Htm][ControlMode]") {
  auto cmd = parseControlCommand("split-window -h -t %1 -c /tmp");
  REQUIRE(cmd.name == "split-window");
  REQUIRE(cmd.flags.has('h'));
  REQUIRE(cmd.flags.get('t') == "%1");
  REQUIRE(cmd.flags.get('c') == "/tmp");
}

TEST_CASE("encodeSendKeys maps named keys", "[Htm][ControlMode]") {
  REQUIRE(encodeSendKeys({"hello", "Enter"}, false, false) == "hello\r");
  REQUIRE(encodeSendKeys({"61"}, true, false) == "a");
  REQUIRE(encodeSendKeys({"0xd"}, false, false) == "\r");
  REQUIRE(encodeSendKeys({"0x0d"}, false, false) == "\r");
  REQUIRE(encodeSendKeys({"H", "i"}, false, true) == "Hi");
}

TEST_CASE("PaneScreen captures fed text", "[Htm][PaneScreen]") {
  PaneScreen screen(40, 10);
  screen.feed("hello\r\n");
  string cap = screen.capture(false, false, 0, 0, false, false);
  REQUIRE(cap.find("hello") != string::npos);
  int x = 0, y = 0;
  screen.cursor(&x, &y);
  REQUIRE(y >= 0);
}

TEST_CASE("PaneScreen capture-pane -e includes SGR and -a is empty off alt",
          "[Htm][PaneScreen]") {
  PaneScreen screen(40, 10);
  screen.feed("\x1b[31mred\x1b[0m\r\n");
  string withEsc = screen.capture(true, false, 0, 0, false, false);
  REQUIRE(withEsc.find("red") != string::npos);
  REQUIRE(withEsc.find("\x1b[") != string::npos);
  REQUIRE(screen.capture(false, true, 0, 0, false, false).empty());
}

TEST_CASE("layout checksum is stable for a given layout body",
          "[Htm][MultiplexerState]") {
  skipIfThreadSanitizer();
  MultiplexerState mux;
  string layout = mux.dumpLayout(mux.activeWindowId(), false);
  REQUIRE(layout.size() >= 5);
  REQUIRE(layout[4] == ',');
  mux.stopAll();
}

TEST_CASE("displayFormat expands window_active conditionals",
          "[Htm][MultiplexerState]") {
  skipIfThreadSanitizer();
  MultiplexerState mux;
  string fmt = "#{window_id}\t#{?window_active,1,0}\t#{pane-border-status}";
  string out = mux.displayFormat(fmt, mux.activeSessionId(),
                                 mux.activeWindowId(), mux.activePaneId());
  REQUIRE(out.find("@") != string::npos);
  REQUIRE(out.find("\t1\t") != string::npos);
  REQUIRE(out.find("off") != string::npos);
  mux.stopAll();
}

TEST_CASE("parseControlCommand swap-pane and set user options",
          "[Htm][ControlMode]") {
  auto swap = parseControlCommand("swap-pane -s %0 -t %1");
  REQUIRE(swap.name == "swap-pane");
  REQUIRE(swap.flags.get('s') == "%0");
  REQUIRE(swap.flags.get('t') == "%1");
  auto setCmd = parseControlCommand("set -t $1 @iterm2_id uuid-here");
  REQUIRE(setCmd.name == "set");
  REQUIRE(setCmd.flags.get('t') == "$1");
  REQUIRE(setCmd.flags.positional.size() >= 2);
  REQUIRE(setCmd.flags.positional[0] == "@iterm2_id");
  REQUIRE(setCmd.flags.positional[1] == "uuid-here");
  auto showCmd = parseControlCommand("show-options -v -q -p -t %0 @uservars");
  REQUIRE(showCmd.flags.has('v'));
  REQUIRE(showCmd.flags.has('q'));
  REQUIRE(showCmd.flags.has('p'));
  REQUIRE(showCmd.flags.get('t') == "%0");
  REQUIRE(showCmd.flags.positional[0] == "@uservars");
  auto brk = parseControlCommand("break-pane -P -F #{window_id} -s %1");
  REQUIRE(brk.flags.has('P'));
  REQUIRE(brk.flags.get('F') == "#{window_id}");
  REQUIRE(brk.flags.get('s') == "%1");
}

TEST_CASE("executeControlCommand stores session and pane user options",
          "[Htm][ControlMode]") {
  skipIfThreadSanitizer();
  MultiplexerState mux;
  ControlWriter writer;
  REQUIRE(
      executeControlCommand(&mux, &writer, "set -t $1 @iterm2_id uuid-abc") ==
      ControlAction::None);
  REQUIRE(mux.getUserOption(' ', mux.activeSessionId(), "@iterm2_id") ==
          "uuid-abc");
  string pane = "%" + to_string(mux.activePaneId());
  REQUIRE(executeControlCommand(&mux, &writer,
                                "set -p -t " + pane + " @uservars foo=bar") ==
          ControlAction::None);
  REQUIRE(mux.getUserOption('p', mux.activePaneId(), "@uservars") == "foo=bar");
  mux.stopAll();
}
