#include "Headers.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("replace replaces first occurrence", "[StringUtils]") {
  std::string str = "hello world, hello universe";
  bool result = replace(str, "hello", "hi");

  REQUIRE(result == true);
  REQUIRE(str == "hi world, hello universe");
}

TEST_CASE("replace returns false when pattern not found", "[StringUtils]") {
  std::string str = "hello world";
  bool result = replace(str, "goodbye", "hi");

  REQUIRE(result == false);
  REQUIRE(str == "hello world");
}

TEST_CASE("replace handles empty string", "[StringUtils]") {
  std::string str = "";
  bool result = replace(str, "hello", "hi");

  REQUIRE(result == false);
  REQUIRE(str == "");
}

TEST_CASE("replaceAll replaces all occurrences", "[StringUtils]") {
  std::string str = "hello world, hello universe, hello everyone";
  int count = replaceAll(str, "hello", "hi");

  REQUIRE(count == 3);
  REQUIRE(str == "hi world, hi universe, hi everyone");
}

TEST_CASE("replaceAll handles no matches", "[StringUtils]") {
  std::string str = "hello world";
  int count = replaceAll(str, "goodbye", "hi");

  REQUIRE(count == 0);
  REQUIRE(str == "hello world");
}

TEST_CASE("replaceAll returns 0 for empty pattern", "[StringUtils]") {
  std::string str = "hello world";
  int count = replaceAll(str, "", "hi");

  REQUIRE(count == 0);
  REQUIRE(str == "hello world");
}

TEST_CASE("replaceAll handles overlapping replacement", "[StringUtils]") {
  std::string str = "xxx";
  int count = replaceAll(str, "x", "yx");

  REQUIRE(count == 3);
  REQUIRE(str == "yxyxyx");
}

TEST_CASE("replaceAll handles replacement with pattern substring",
          "[StringUtils]") {
  std::string str = "aaaa";
  int count = replaceAll(str, "aa", "a");

  REQUIRE(count == 2);
  REQUIRE(str == "aa");
}
