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
