#include <cxxopts.hpp>

#include "ClientArgParsing.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {

struct ParsedConnect {
  cxxopts::ParseResult result;
  vector<string> commandOperands;
};

ParsedConnect parseEtConnectArgv(int argc, const char** argv) {
  vector<string> rawArgs;
  rawArgs.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    rawArgs.emplace_back(argv[i]);
  }
  EtArgvSplit split = splitEtArgvAtHost(rawArgs);
  vector<char*> clientArgv;
  clientArgv.reserve(split.clientArgs.size());
  for (auto& arg : split.clientArgs) {
    clientArgv.push_back(&arg[0]);
  }

  cxxopts::Options options("et", "Remote shell for the busy and impatient");
  options.allow_unrecognised_options();
  options.add_options()("p,port", "Remote machine etserver port",
                        cxxopts::value<int>()->default_value("2022"))(
      "c,command", "Run command on connect and exit after command is run",
      cxxopts::value<std::string>())("terminal-path", "Path to etterminal",
                                     cxxopts::value<std::string>())(
      "serverfifo", "Server fifo", cxxopts::value<std::string>())(
      "F,ssh-config", "SSH config file", cxxopts::value<std::string>())(
      "o", "OpenSSH-style session option",
      cxxopts::value<std::vector<std::string>>())(
      "disconnect-timeout", "Session disconnect timeout minutes",
      cxxopts::value<int>())("logtostdout", "Write log to stdout")(
      "host", "Remote host name", cxxopts::value<std::string>());
  options.parse_positional({"host"});
  ParsedConnect parsed{
      options.parse(static_cast<int>(clientArgv.size()), clientArgv.data()),
      split.commandOperands};
  return parsed;
}

string commandFromParse(const ParsedConnect& parsed) {
  return resolveRemoteCommand(
      parsed.commandOperands, parsed.result.count("command") > 0,
      parsed.result.count("command") ? parsed.result["command"].as<string>()
                                     : "");
}

}  // namespace

TEST_CASE("et accepts ssh-style positional remote command",
          "[ClientArgParsing]") {
  SECTION("positional command after user@host") {
    const char* argv[] = {"et",   "-p",    "2022", "user@host",
                          "echo", "hello", "world"};
    auto parsed = parseEtConnectArgv(7, argv);
    auto& result = parsed.result;
    REQUIRE(result.count("host") == 1);
    REQUIRE(result["port"].as<int>() == 2022);

    ParsedEtDestination dest =
        parseEtDestinationHost(result["host"].as<string>());
    REQUIRE(dest.username == "user");
    REQUIRE(dest.host == "host");
    REQUIRE_FALSE(dest.hasExplicitPort);

    REQUIRE(commandFromParse(parsed) == "echo hello world");
  }

  SECTION("-c works when no positional command is present") {
    const char* argv[] = {"et", "-c", "ls -la", "user@host"};
    auto parsed = parseEtConnectArgv(4, argv);
    auto& result = parsed.result;
    REQUIRE(result.count("host") == 1);

    ParsedEtDestination dest =
        parseEtDestinationHost(result["host"].as<string>());
    REQUIRE(dest.username == "user");
    REQUIRE(dest.host == "host");

    REQUIRE(commandFromParse(parsed) == "ls -la");
  }

  SECTION("positional command wins over -c") {
    const char* argv[] = {"et", "-c", "ignored", "host", "echo", "ok"};
    auto parsed = parseEtConnectArgv(6, argv);
    REQUIRE(commandFromParse(parsed) == "echo ok");
  }

  SECTION("unknown dash operands after the host stay in the command") {
    const char* argv[] = {"et", "host", "echo", "-n", "hi"};
    auto parsed = parseEtConnectArgv(5, argv);
    auto& result = parsed.result;
    REQUIRE(result.count("host") == 1);
    REQUIRE(result["host"].as<string>() == "host");
    REQUIRE(commandFromParse(parsed) == "echo -n hi");
  }

  SECTION("equals-style options are not the host") {
    const char* argv[] = {"et",
                          "-c",
                          "echo 'compat new to old'",
                          "--serverfifo=/tmp/etserver.compat.fifo",
                          "--terminal-path",
                          "/build/etterminal",
                          "--logtostdout",
                          "localhost:9920"};
    auto parsed = parseEtConnectArgv(8, argv);
    REQUIRE(parsed.result.count("host") == 1);
    REQUIRE(parsed.result["host"].as<string>() == "localhost:9920");
    REQUIRE(commandFromParse(parsed) == "echo 'compat new to old'");
    ParsedEtDestination dest =
        parseEtDestinationHost(parsed.result["host"].as<string>());
    REQUIRE(dest.host == "localhost");
    REQUIRE(dest.hasExplicitPort);
    REQUIRE(dest.port == 9920);
  }

  SECTION("et options after the host are remote operands") {
    const char* argv[] = {"et", "host", "sh", "-c", "echo hi"};
    auto parsed = parseEtConnectArgv(5, argv);
    auto& result = parsed.result;
    REQUIRE(result.count("host") == 1);
    REQUIRE(result["host"].as<string>() == "host");
    REQUIRE(result.count("command") == 0);
    REQUIRE(commandFromParse(parsed) == "sh -c echo hi");
  }

  SECTION("-- before host with positional command") {
    const char* argv[] = {"et", "--", "host", "cmd"};
    auto parsed = parseEtConnectArgv(4, argv);
    auto& result = parsed.result;
    REQUIRE(result.count("host") == 1);
    REQUIRE(result["host"].as<string>() == "host");
    REQUIRE(commandFromParse(parsed) == "cmd");
  }

  SECTION("-- before host without positional command") {
    const char* argv[] = {"et", "--", "host"};
    auto parsed = parseEtConnectArgv(3, argv);
    auto& result = parsed.result;
    REQUIRE(result.count("host") == 1);
    REQUIRE(result["host"].as<string>() == "host");
    REQUIRE(commandFromParse(parsed) == "");
  }

  SECTION("-c before -- still leaves host for cxxopts") {
    const char* argv[] = {"et", "-c", "ls", "--", "host"};
    auto parsed = parseEtConnectArgv(5, argv);
    auto& result = parsed.result;
    REQUIRE(result.count("host") == 1);
    REQUIRE(result["host"].as<string>() == "host");
    REQUIRE(commandFromParse(parsed) == "ls");
  }

  SECTION("token after -- is host even if it starts with -") {
    const char* argv[] = {"et", "--", "-weirdhost", "echo", "ok"};
    auto parsed = parseEtConnectArgv(5, argv);
    auto& result = parsed.result;
    REQUIRE(result.count("host") == 1);
    REQUIRE(result["host"].as<string>() == "-weirdhost");
    REQUIRE(commandFromParse(parsed) == "echo ok");
  }

  SECTION("-- after the host stays a remote command operand") {
    const char* argv[] = {"et", "host", "--", "cmd"};
    auto parsed = parseEtConnectArgv(4, argv);
    auto& result = parsed.result;
    REQUIRE(result.count("host") == 1);
    REQUIRE(result["host"].as<string>() == "host");
    REQUIRE(commandFromParse(parsed) == "-- cmd");
  }

  SECTION("-F and -o consume values so the host stays positional") {
    const char* argv[] = {"et",
                          "-G",
                          "-F",
                          "/tmp/ssh_config",
                          "-o",
                          "ConnectTimeout=9",
                          "-o",
                          "RemoteCommand=true",
                          "nowhere.invalid"};
    auto parsed = parseEtConnectArgv(9, argv);
    auto& result = parsed.result;
    REQUIRE(result.count("host") == 1);
    REQUIRE(result["host"].as<string>() == "nowhere.invalid");
    REQUIRE(result.count("ssh-config") == 1);
    REQUIRE(result["ssh-config"].as<string>() == "/tmp/ssh_config");
    REQUIRE(result.count("o") == 2);
    auto sessionOpts = result["o"].as<std::vector<string>>();
    REQUIRE(sessionOpts.size() == 2);
    REQUIRE(sessionOpts[0] == "ConnectTimeout=9");
    REQUIRE(sessionOpts[1] == "RemoteCommand=true");
    REQUIRE(commandFromParse(parsed) == "");
  }

  SECTION("--disconnect-timeout consumes its value before the host") {
    const char* argv[] = {"et", "--disconnect-timeout", "10080", "user@host"};
    auto parsed = parseEtConnectArgv(4, argv);
    auto& result = parsed.result;
    REQUIRE(result.count("host") == 1);
    REQUIRE(result["host"].as<string>() == "user@host");
    REQUIRE(result.count("disconnect-timeout") == 1);
    REQUIRE(result["disconnect-timeout"].as<int>() == 10080);
    REQUIRE(commandFromParse(parsed) == "");
  }

  SECTION("--disconnect-timeout=minutes keeps the host positional") {
    const char* argv[] = {"et", "--disconnect-timeout=10080", "host", "echo",
                          "hi"};
    auto parsed = parseEtConnectArgv(5, argv);
    REQUIRE(parsed.result.count("host") == 1);
    REQUIRE(parsed.result["host"].as<string>() == "host");
    REQUIRE(parsed.result.count("disconnect-timeout") == 1);
    REQUIRE(parsed.result["disconnect-timeout"].as<int>() == 10080);
    REQUIRE(commandFromParse(parsed) == "echo hi");
  }
}

TEST_CASE("joinRemoteCommandOperands preserves argv words",
          "[ClientArgParsing]") {
  REQUIRE(joinRemoteCommandOperands({"echo", "hello", "world"}) ==
          "echo hello world");
  REQUIRE(joinRemoteCommandOperands({}) == "");
  REQUIRE(joinRemoteCommandOperands({"alone"}) == "alone");
}
