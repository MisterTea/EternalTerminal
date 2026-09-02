#include "SubprocessUtils.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("SubprocessUtils SubprocessToStringInteractive executes command",
          "[SubprocessUtils]") {
  // Test simple echo command
  SubprocessUtils utils;
#ifdef WIN32
  string result = utils.SubprocessToStringInteractive(
      "cmd.exe", {"/D", "/S", "/C", "\"echo hello world\""});
#else
  string result =
      utils.SubprocessToStringInteractive("echo", {"hello", "world"});
#endif

  // The output should contain "hello world" (with possible whitespace/newline)
  REQUIRE(result.find("hello") != string::npos);
  REQUIRE(result.find("world") != string::npos);
}

TEST_CASE("SubprocessUtils SubprocessToStringInteractive with no args",
          "[SubprocessUtils]") {
  // Test command with no arguments
  SubprocessUtils utils;
  string result;
#ifdef WIN32
  result = utils.SubprocessToStringInteractive("cmd.exe", {"/D", "/C", "cd"});
#else
  result = utils.SubprocessToStringInteractive("pwd", {});
#endif

  // pwd should return a path (containing at least a forward slash)
#ifdef WIN32
  REQUIRE(result.find(":\\") != string::npos);
#else
  REQUIRE(result.find("/") != string::npos);
#endif
}

TEST_CASE("SubprocessUtils SubprocessToStringInteractive captures stdout",
          "[SubprocessUtils]") {
  // Test that we capture stdout properly
  SubprocessUtils utils;
#ifdef WIN32
  string result = utils.SubprocessToStringInteractive(
      "cmd.exe", {"/D", "/S", "/C", "\"<nul set /p =test123\""});
#else
  string result = utils.SubprocessToStringInteractive("printf", {"test123"});
#endif

  REQUIRE(result == "test123");
}

#ifdef WIN32
TEST_CASE("SubprocessUtils reports CreateProcess failures",
          "[SubprocessUtils]") {
  SubprocessUtils utils;
  REQUIRE_THROWS_WITH(
      utils.SubprocessToStringInteractive(
          "et-command-that-does-not-exist-7f5d63.exe", {}),
      Catch::Matchers::ContainsSubstring("CreateProcess failed with error"));
}
#endif
