#include "SubprocessUtils.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace et;

TEST_CASE("SubprocessUtils SubprocessToStringInteractive executes command",
          "[SubprocessUtils]") {
  // Test simple echo command
  SubprocessUtils utils;
  string result = utils.SubprocessToStringInteractive("echo", {"hello", "world"});

  // The output should contain "hello world" (with possible whitespace/newline)
  REQUIRE(result.find("hello") != string::npos);
  REQUIRE(result.find("world") != string::npos);
}

TEST_CASE("SubprocessUtils SubprocessToStringInteractive with no args",
          "[SubprocessUtils]") {
  // Test command with no arguments
  SubprocessUtils utils;
  string result = utils.SubprocessToStringInteractive("pwd", {});

  // pwd should return a path (containing at least a forward slash)
  REQUIRE(result.find("/") != string::npos);
}

TEST_CASE("SubprocessUtils SubprocessToStringInteractive captures stdout",
          "[SubprocessUtils]") {
  // Test that we capture stdout properly
  SubprocessUtils utils;
  string result = utils.SubprocessToStringInteractive("printf", {"test123"});

  REQUIRE(result == "test123");
}
