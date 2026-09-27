#include "TerminalStdinParsing.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("parseTerminalStdinLine", "[TerminalStdinParsing]") {
  TerminalStdinLine result;

  SECTION("Plain TERM") {
    REQUIRE(parseTerminalStdinLine("abc/def_xterm-256color", &result));
    REQUIRE(result.idpasskey == "abc/def");
    REQUIRE(result.term == "xterm-256color");
  }

  SECTION("New-client placeholder") {
    REQUIRE(parseTerminalStdinLine("XXX/YYY_screen", &result));
    REQUIRE(result.idpasskey == "XXX/YYY");
    REQUIRE(result.term == "screen");
  }

  SECTION("TERM containing underscores") {
    REQUIRE(parseTerminalStdinLine("abc/def_xterm_256color", &result));
    REQUIRE(result.idpasskey == "abc/def");
    REQUIRE(result.term == "xterm_256color");

    REQUIRE(parseTerminalStdinLine("abc/def_a_b_c", &result));
    REQUIRE(result.term == "a_b_c");

    // split() dropped a trailing empty token, which stripped this underscore.
    REQUIRE(parseTerminalStdinLine("abc/def_xterm_", &result));
    REQUIRE(result.term == "xterm_");
  }

  SECTION("Malformed lines") {
    REQUIRE_FALSE(parseTerminalStdinLine("", &result));
    REQUIRE_FALSE(parseTerminalStdinLine("abc/def", &result));
    REQUIRE_FALSE(parseTerminalStdinLine("abc/def_", &result));
    REQUIRE_FALSE(parseTerminalStdinLine("_xterm", &result));
  }
}
