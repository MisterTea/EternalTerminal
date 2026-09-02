#include "HtmTestHelpers.hpp"
#include "MultiplexerState.hpp"
#include "TestHeaders.hpp"

using namespace et;
using namespace et::htmtest;

TEST_CASE("MultiplexerState starts with one session, window, and pane",
          "[Htm][MultiplexerState]") {
  skipIfThreadSanitizer();
  MultiplexerState mux;
  REQUIRE(mux.numPanes() == 1);
  REQUIRE(mux.activeSessionId() != 0);
  REQUIRE(mux.activeWindowId() != 0);
  REQUIRE(mux.activePaneId() == 0);
  string layout = mux.dumpLayout(mux.activeWindowId(), false);
  REQUIRE(layout.find("x") != string::npos);
  mux.resizePaneDir(mux.activePaneId(), 'L', 1);
  mux.stopAll();
}

TEST_CASE("MultiplexerState splits, nested splits, windows, and close",
          "[Htm][MultiplexerState]") {
  skipIfThreadSanitizer();
  MultiplexerState mux;
  uint32_t pane1 = mux.activePaneId();
  uint32_t pane2 = mux.splitWindow(pane1, true, "");
  REQUIRE(mux.numPanes() == 2);
  uint32_t pane3 = mux.splitWindow(pane1, true, "");
  REQUIRE(mux.numPanes() == 3);
  uint32_t pane4 = mux.splitWindow(pane2, false, "");
  REQUIRE(mux.numPanes() == 4);
  string layout = mux.dumpLayout(mux.activeWindowId(), false);
  REQUIRE(layout.find("{") != string::npos);
  REQUIRE(layout.find("[") != string::npos);

  uint32_t win2 = mux.newWindow("two", "");
  REQUIRE(mux.hasWindow(win2));
  mux.selectWindow(win2);
  REQUIRE(mux.activeWindowId() == win2);

  mux.closePane(pane4);
  mux.closePane(pane3);
  mux.closePane(pane2);
  mux.closeWindow(win2);
  mux.stopAll();
}

TEST_CASE("MultiplexerState sessions rename attach and list",
          "[Htm][MultiplexerState]") {
  skipIfThreadSanitizer();
  MultiplexerState mux;
  uint32_t extra = mux.newSession("other");
  REQUIRE(mux.hasSession(extra));
  mux.renameSession(extra, "renamed");
  mux.attachSession(extra);
  REQUIRE(mux.activeSessionId() == extra);
  string listed = mux.listSessions("#{session_id} #{session_name}");
  REQUIRE(listed.find("renamed") != string::npos);
  mux.closeSession(extra);
  mux.stopAll();
}

TEST_CASE("MultiplexerState zoom and capture", "[Htm][MultiplexerState]") {
  skipIfThreadSanitizer();
  MultiplexerState mux;
  uint32_t pane = mux.activePaneId();
  uint32_t split = mux.splitWindow(pane, false, "");
  mux.zoomToggle(split);
  string visible = mux.dumpLayout(mux.activeWindowId(), true);
  REQUIRE(visible.find(to_string(split)) != string::npos);
  mux.zoomToggle(split);
#ifdef WIN32
  mux.sendKeys(split, "echo MUX_ECHO_99\r\n");
#else
  mux.sendKeys(split, "printf 'MUX_ECHO_99\\n'\n");
#endif
  REQUIRE(waitUntil(
      [&]() {
        mux.pollOutput();
        return mux.capturePane(split, false, false, -2000, -1, true, true)
                   .find("MUX_ECHO_99") != string::npos;
      },
      8000));
  mux.stopAll();
}

TEST_CASE("MultiplexerState swap-pane exchanges layout order",
          "[Htm][MultiplexerState]") {
  skipIfThreadSanitizer();
  MultiplexerState mux;
  uint32_t p1 = mux.activePaneId();
  uint32_t p2 = mux.splitWindow(p1, false, "");
  string before = mux.listPanes("#{pane_id}", mux.activeWindowId());
  REQUIRE(before.find("%" + to_string(p1)) < before.find("%" + to_string(p2)));
  mux.swapPanes(p1, p2);
  string after = mux.listPanes("#{pane_id}", mux.activeWindowId());
  REQUIRE(after.find("%" + to_string(p2)) < after.find("%" + to_string(p1)));
  REQUIRE(mux.hasPane(p1));
  REQUIRE(mux.hasPane(p2));
  mux.stopAll();
}

TEST_CASE("MultiplexerState move-pane join break and unlink",
          "[Htm][MultiplexerState]") {
  skipIfThreadSanitizer();
  MultiplexerState mux;
  uint32_t p1 = mux.activePaneId();
  uint32_t w1 = mux.activeWindowId();
  uint32_t p2 = mux.splitWindow(p1, true, "");
  uint32_t w2 = mux.newWindow("two", "");
  uint32_t p3 = mux.activePaneId();
  mux.movePane(p2, p3, false, false);
  REQUIRE(mux.hasPane(p1));
  REQUIRE(mux.hasPane(p2));
  REQUIRE(mux.hasPane(p3));
  REQUIRE(mux.hasWindow(w1));
  mux.selectWindow(w1);
  REQUIRE(mux.activePaneId() == p1);
  string destLayout = mux.dumpLayout(w2, false);
  REQUIRE(destLayout.find("," + to_string(p2)) != string::npos);
  REQUIRE(destLayout.find("," + to_string(p3)) != string::npos);
  REQUIRE(mux.dumpLayout(w1, false).find("," + to_string(p2)) == string::npos);

  uint32_t broken = mux.breakPane(p2);
  REQUIRE(broken != w2);
  REQUIRE(mux.hasWindow(broken));
  REQUIRE(mux.dumpLayout(broken, false).find("," + to_string(p2)) !=
          string::npos);
  REQUIRE(mux.dumpLayout(w2, false).find("," + to_string(p2)) == string::npos);

  mux.closeWindow(broken);
  REQUIRE(!mux.hasWindow(broken));
  mux.stopAll();
}

TEST_CASE("MultiplexerState move last pane closes the source window",
          "[Htm][MultiplexerState]") {
  skipIfThreadSanitizer();
  MultiplexerState mux;
  uint32_t p1 = mux.activePaneId();
  uint32_t w1 = mux.activeWindowId();
  uint32_t w2 = mux.newWindow("two", "");
  uint32_t p2 = mux.activePaneId();
  mux.movePane(p1, p2, true, true);
  REQUIRE(!mux.hasWindow(w1));
  REQUIRE(mux.hasPane(p1));
  REQUIRE(mux.hasPane(p2));
  string layout = mux.dumpLayout(w2, false);
  REQUIRE(layout.find("," + to_string(p1)) != string::npos);
  REQUIRE(layout.find("," + to_string(p2)) != string::npos);
  mux.stopAll();
}

TEST_CASE("MultiplexerState user options persist on session and pane",
          "[Htm][MultiplexerState]") {
  skipIfThreadSanitizer();
  MultiplexerState mux;
  uint32_t sid = mux.activeSessionId();
  uint32_t pane = mux.activePaneId();
  mux.setUserOption(' ', sid, "@iterm2_id", "guid-1");
  REQUIRE(mux.getUserOption(' ', sid, "@iterm2_id") == "guid-1");
  mux.setUserOption('p', pane, "@uservars", "a=b");
  REQUIRE(mux.getUserOption('p', pane, "@uservars") == "a=b");
  mux.setUserOption('p', pane, "@uservars", ",c=d", true);
  REQUIRE(mux.getUserOption('p', pane, "@uservars") == "a=b,c=d");
  mux.unsetUserOption(' ', sid, "@iterm2_id");
  REQUIRE(mux.getUserOption(' ', sid, "@iterm2_id").empty());
  mux.setUserOption(' ', sid, "status", "off");
  REQUIRE(mux.getUserOption(' ', sid, "status").empty());
  mux.stopAll();
}
