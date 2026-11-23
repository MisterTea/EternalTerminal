#include "TestHeaders.hpp"
#include "TimeParser.hpp"

using namespace et;

TEST_CASE("TimeParser tests") {
  SECTION("invalid") {
    CHECK(parseTimeString("", -1) == -1);
    CHECK(parseTimeString(" ", -2) == -2);
    CHECK(parseTimeString("-1", -1) == -1);
    CHECK(parseTimeString("1x", -1) == -1);
    CHECK(parseTimeString("1h30m ", -1) == -1);
    CHECK(parseTimeString(" 1h30m", -1) == -1);
    CHECK(parseTimeString("15250284452472w", -1) == -1);
    CHECK(parseTimeString("9223372036854775807s1s", -1) == -1);
    CHECK(parseTimeString("9223372036854775808s", -1) == -1);
    if constexpr (sizeof(long) == 4) {
      CHECK(parseTimeString("2147483648", -1) == -1);
    }
  }

  SECTION("valid") {
    CHECK(parseTimeString("1", -1) == 1);
    CHECK(parseTimeString("1s", -1) == 1);
    CHECK(parseTimeString("1S", -1) == 1);
    CHECK(parseTimeString("1m", -1) == 60);
    CHECK(parseTimeString("1M", -1) == 60);
    CHECK(parseTimeString("1h", -1) == 3600);
    CHECK(parseTimeString("1H", -1) == 3600);
    CHECK(parseTimeString("1d", -1) == 86400);
    CHECK(parseTimeString("1D", -1) == 86400);
    CHECK(parseTimeString("1w", -1) == 604800);
    CHECK(parseTimeString("1W", -1) == 604800);
    CHECK(parseTimeString("1m30s", -1) == 90);
    CHECK(parseTimeString("1h30m", -1) == 5400);
    CHECK(parseTimeString("2w3d1h45m1s", -1) == 1475101);
  }
}
