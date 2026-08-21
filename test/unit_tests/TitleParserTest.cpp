#include "TestHeaders.hpp"
#include "TitleParser.hpp"

using namespace et;

TEST_CASE("TitleParser recognizes OSC title sequences", "[TitleParser]") {
  TitleParser parser;

  REQUIRE(parser.parse("before\033]0;zero\007after") ==
          optional<string>("zero"));
  REQUIRE(parser.parse("\033]2;two\033\\") == optional<string>("two"));
}

TEST_CASE("TitleParser survives sequences split across chunks",
          "[TitleParser]") {
  TitleParser parser;

  REQUIRE_FALSE(parser.parse("\033").has_value());
  REQUIRE_FALSE(parser.parse("]2;split").has_value());
  REQUIRE_FALSE(parser.parse(" title\033").has_value());
  REQUIRE(parser.parse("\\") == optional<string>("split title"));
}

TEST_CASE("TitleParser ignores non-title OSC and resynchronizes",
          "[TitleParser]") {
  TitleParser parser;

  REQUIRE_FALSE(parser.parse("\033]7;file:///tmp\007").has_value());
  REQUIRE_FALSE(parser.parse("\033]52;c;clipboard\033\\").has_value());
  REQUIRE(parser.parse("\033]2;discarded\033]7;cwd\007\033]2;kept\007") ==
          optional<string>("kept"));
}

TEST_CASE("TitleParser sanitizes and bounds titles", "[TitleParser]") {
  TitleParser parser;

  REQUIRE(parser.parse("\033]2;a\001b\177c\007") == optional<string>("abc"));

  const string longTitle = string(79, 'a') + "\xC3\xA9";
  const optional<string> parsed = parser.parse("\033]2;" + longTitle + "\007");
  REQUIRE(parsed.has_value());
  REQUIRE(*parsed == string(79, 'a'));
  REQUIRE(parsed->size() <= 80);
}

TEST_CASE("TitleParser removes C1 controls without damaging UTF-8",
          "[TitleParser]") {
  TitleParser parser;

  const string title =
      "cl\xC3\xA9"  // valid U+00E9
      "\xC2\x80"    // encoded U+0080 control
      "ok"
      "\xC2\x9F"           // encoded U+009F control
      " \xF0\x9F\x98\x80"  // valid U+1F600
      "\x80"               // invalid continuation byte
      "\xC3x";             // invalid two-byte sequence, then ASCII x

  REQUIRE(parser.parse("\033]2;" + title + "\007") ==
          optional<string>("cl\xC3\xA9ok \xF0\x9F\x98\x80x"));
}

TEST_CASE("TitleParser reports an empty title as a clear", "[TitleParser]") {
  TitleParser parser;
  const optional<string> parsed = parser.parse("\033]2;\007");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->empty());
}
