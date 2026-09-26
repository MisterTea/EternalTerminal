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

#ifndef WIN32
TEST_CASE("SubprocessToStringInteractive does not consume parent stdin",
          "[SubprocessUtils]") {
  int fds[2];
  REQUIRE(pipe(fds) == 0);
  const char secret[] = "SECRET_STDIN";
  REQUIRE(write(fds[1], secret, sizeof(secret) - 1) ==
          static_cast<ssize_t>(sizeof(secret) - 1));
  REQUIRE(close(fds[1]) == 0);
  int savedStdin = dup(STDIN_FILENO);
  REQUIRE(savedStdin >= 0);
  REQUIRE(dup2(fds[0], STDIN_FILENO) == STDIN_FILENO);
  REQUIRE(close(fds[0]) == 0);

  SubprocessUtils utils;
  string result = utils.SubprocessToStringInteractive("cat", {});

  REQUIRE(dup2(savedStdin, STDIN_FILENO) == STDIN_FILENO);
  REQUIRE(close(savedStdin) == 0);
  REQUIRE(result.empty());
}
#endif

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
