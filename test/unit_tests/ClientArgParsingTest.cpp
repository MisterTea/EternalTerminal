#include <cxxopts.hpp>

#include "ClientArgParsing.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {

cxxopts::ParseResult parseEtConnectArgv(int argc, const char** argv) {
  cxxopts::Options options("et", "Remote shell for the busy and impatient");
  options.allow_unrecognised_options();
  options.add_options()("p,port", "Remote machine etserver port",
                        cxxopts::value<int>()->default_value("2022"))(
      "c,command", "Run command on connect and exit after command is run",
      cxxopts::value<std::string>())("host", "Remote host name",
                                     cxxopts::value<std::string>())(
      "command_args", "Positional remote command words",
      cxxopts::value<std::vector<std::string>>());
  options.parse_positional({"host", "command_args"});
  return options.parse(argc, const_cast<char**>(argv));
}

string commandFromParse(const cxxopts::ParseResult& result) {
  vector<string> positional;
  if (result.count("command_args")) {
    positional = result["command_args"].as<vector<string>>();
  }
  return resolveRemoteCommand(
      positional, result.count("command") > 0,
      result.count("command") ? result["command"].as<string>() : "");
}

}  // namespace

TEST_CASE("et accepts ssh-style positional remote command",
          "[ClientArgParsing]") {
  SECTION("positional command after user@host") {
    const char* argv[] = {"et",   "-p",    "2022", "user@host",
                          "echo", "hello", "world"};
    auto result = parseEtConnectArgv(7, argv);
    REQUIRE(result.count("host") == 1);
    REQUIRE(result["port"].as<int>() == 2022);

    ParsedEtDestination dest =
        parseEtDestinationHost(result["host"].as<string>());
    REQUIRE(dest.username == "user");
    REQUIRE(dest.host == "host");
    REQUIRE_FALSE(dest.hasExplicitPort);

    REQUIRE(commandFromParse(result) == "echo hello world");
  }

  SECTION("-c works when no positional command is present") {
    const char* argv[] = {"et", "-c", "ls -la", "user@host"};
    auto result = parseEtConnectArgv(4, argv);
    REQUIRE(result.count("host") == 1);

    ParsedEtDestination dest =
        parseEtDestinationHost(result["host"].as<string>());
    REQUIRE(dest.username == "user");
    REQUIRE(dest.host == "host");

    REQUIRE(commandFromParse(result) == "ls -la");
  }

  SECTION("positional command wins over -c") {
    const char* argv[] = {"et", "-c", "ignored", "host", "echo", "ok"};
    auto result = parseEtConnectArgv(6, argv);
    REQUIRE(commandFromParse(result) == "echo ok");
  }
}

TEST_CASE("joinRemoteCommandOperands preserves argv words",
          "[ClientArgParsing]") {
  REQUIRE(joinRemoteCommandOperands({"echo", "hello", "world"}) ==
          "echo hello world");
  REQUIRE(joinRemoteCommandOperands({}) == "");
  REQUIRE(joinRemoteCommandOperands({"alone"}) == "alone");
}
