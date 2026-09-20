#include "SubprocessUtils.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("SubprocessToStringInteractive tees stderr to stdout (banner)",
          "[SubprocessUtils]") {
  SubprocessUtils utils;
  // Write a message to stderr only (no stdout). The tee fix should
  // redirect stderr to the same pipe as stdout so callers (like the
  // SSH banner extractor) see it.
#ifdef WIN32
  string result = utils.SubprocessToStringInteractive(
      "cmd.exe", {"/D", "/S", "/C", "echo banner message 1>&2"});
#else
  string result = utils.SubprocessToStringInteractive(
      "sh", {"-c", "echo banner message >&2"});
#endif
  REQUIRE(result.find("banner message") != string::npos);
}

TEST_CASE("SubprocessToStringInteractive captures stdout and stderr",
          "[SubprocessUtils]") {
  SubprocessUtils utils;
#ifdef WIN32
  string result = utils.SubprocessToStringInteractive(
      "cmd.exe", {"/D", "/S", "/C", "echo stdout & echo stderr 1>&2"});
#else
  string result = utils.SubprocessToStringInteractive(
      "sh", {"-c", "printf stdout; printf stderr >&2"});
#endif
  REQUIRE(result.find("stdout") != string::npos);
  REQUIRE(result.find("stderr") != string::npos);
}
