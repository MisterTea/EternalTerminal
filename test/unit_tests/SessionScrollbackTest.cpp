#include "SessionScrollback.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("ScrollbackAppendReadRoundTrip", "[SessionScrollback]") {
  SessionScrollback sb;
  sb.append("hello ");
  sb.append("world");
  auto r = sb.read(0);
  REQUIRE(r.data == "hello world");
  REQUIRE(r.nextCursor == 11);
  REQUIRE(r.truncated == false);
  REQUIRE(sb.headCursor() == 11);
}

TEST_CASE("ScrollbackCursorReturnsOnlyNewBytes", "[SessionScrollback]") {
  SessionScrollback sb;
  sb.append("abc");
  auto r1 = sb.read(0);
  REQUIRE(r1.data == "abc");

  auto r2 = sb.read(r1.nextCursor);
  REQUIRE(r2.data.empty());
  REQUIRE(r2.nextCursor == 3);
  REQUIRE(r2.truncated == false);

  sb.append("def");
  auto r3 = sb.read(r2.nextCursor);
  REQUIRE(r3.data == "def");
  REQUIRE(r3.nextCursor == 6);
}

TEST_CASE("ScrollbackPartialCursorInsideChunk", "[SessionScrollback]") {
  SessionScrollback sb;
  sb.append("0123456789");
  auto r = sb.read(4);
  REQUIRE(r.data == "456789");
  REQUIRE(r.nextCursor == 10);
}

TEST_CASE("ScrollbackNegativeCursorReadsFromOldest", "[SessionScrollback]") {
  SessionScrollback sb;
  sb.append("xyz");
  auto r = sb.read(-1);
  REQUIRE(r.data == "xyz");
}

TEST_CASE("ScrollbackEvictsPastCapAndFlagsTruncated", "[SessionScrollback]") {
  SessionScrollback sb(10);
  sb.append("AAAAA");
  sb.append("BBBBB");
  sb.append("CCCCC");  // retained would be 15 > 10 => evict "AAAAA"
  REQUIRE(sb.baseCursor() == 5);
  REQUIRE(sb.headCursor() == 15);
  REQUIRE(sb.size() == 10);

  auto r = sb.read(0);  // below base => clamped + truncated
  REQUIRE(r.truncated == true);
  REQUIRE(r.data == "BBBBBCCCCC");
  REQUIRE(r.nextCursor == 15);

  auto r2 = sb.read(10);  // inside the window
  REQUIRE(r2.truncated == false);
  REQUIRE(r2.data == "CCCCC");
}

TEST_CASE("ScrollbackMultiReaderCursorsAreIndependent", "[SessionScrollback]") {
  SessionScrollback sb;
  sb.append("one");
  auto ra = sb.read(0);
  REQUIRE(ra.data == "one");

  sb.append("two");
  auto rb = sb.read(0);  // a fresh reader sees everything
  REQUIRE(rb.data == "onetwo");

  auto ra2 = sb.read(ra.nextCursor);  // the first reader sees only the new bytes
  REQUIRE(ra2.data == "two");
}

TEST_CASE("ScrollbackEmptyAppendIsNoOp", "[SessionScrollback]") {
  SessionScrollback sb;
  sb.append("");
  REQUIRE(sb.headCursor() == 0);
  REQUIRE(sb.read(0).data.empty());
}
