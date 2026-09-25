#include <cxxopts.hpp>

#include "ClientArgParsing.hpp"
#include "MuxProtocol.hpp"
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
  options.add_options()("port", "Remote machine etserver port",
                        cxxopts::value<int>()->default_value("2022"))(
      "command", "Run command on connect and exit after command is run",
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
    const char* argv[] = {"et",   "--port", "2022", "user@host",
                          "echo", "hello",  "world"};
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

  SECTION("--command works when no positional command is present") {
    const char* argv[] = {"et", "--command", "ls -la", "user@host"};
    auto parsed = parseEtConnectArgv(4, argv);
    auto& result = parsed.result;
    REQUIRE(result.count("host") == 1);

    ParsedEtDestination dest =
        parseEtDestinationHost(result["host"].as<string>());
    REQUIRE(dest.username == "user");
    REQUIRE(dest.host == "host");

    REQUIRE(commandFromParse(parsed) == "ls -la");
  }

  SECTION("positional command wins over --command") {
    const char* argv[] = {"et", "--command", "ignored", "host", "echo", "ok"};
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
                          "--command",
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

  SECTION("--command before -- still leaves host for cxxopts") {
    const char* argv[] = {"et", "--command", "ls", "--", "host"};
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

MuxParseResult parseArgv(const vector<string>& storageIn) {
  vector<string> storage = storageIn;
  vector<char*> argv;
  argv.reserve(storage.size());
  for (auto& s : storage) {
    argv.push_back(&s[0]);
  }
  return parseMuxCliOptions(static_cast<int>(argv.size()), argv.data());
}

TEST_CASE("OpenSSH short flags do not keep ET meanings", "[ClientArgParsing]") {
  SECTION("-p is the sshd port and --port stays the etserver port") {
    auto parsed = parseArgv({"et", "-p", "22", "--port", "2023", "host"});
    REQUIRE(parsed.ssh.sshPortSet);
    REQUIRE(parsed.ssh.sshPort == 22);
    EtArgvSplit split = splitEtArgvAtHost(parsed.remainingArgs);
    vector<char*> clientArgv;
    for (auto& arg : split.clientArgs) {
      clientArgv.push_back(&arg[0]);
    }
    cxxopts::Options options("et", "et");
    options.allow_unrecognised_options();
    options.add_options()("port", "etserver port",
                          cxxopts::value<int>()->default_value("2022"))(
        "host", "host", cxxopts::value<string>());
    options.parse_positional({"host"});
    auto result =
        options.parse(static_cast<int>(clientArgv.size()), clientArgv.data());
    REQUIRE(result["port"].as<int>() == 2023);
    REQUIRE(result["host"].as<string>() == "host");
    REQUIRE(split.commandOperands.empty());
  }

  SECTION("-t does not consume the host or create a tunnel") {
    auto parsed = parseArgv({"et", "-t", "host"});
    REQUIRE(parsed.ssh.pty == PtyOverride::Force);
    REQUIRE(parsed.ssh.localForwards.empty());
    REQUIRE(parsed.remainingArgs.size() == 2);
    REQUIRE(parsed.remainingArgs[1] == "host");
  }

  SECTION("--tunnel and -L both describe local forwards") {
    auto parsed = parseArgv(
        {"et", "--tunnel", "18080:80", "-L", "8080:localhost:80", "host"});
    REQUIRE(parsed.ssh.localForwards.size() == 1);
    REQUIRE(parsed.ssh.localForwards[0] == "8080:localhost:80");
    REQUIRE(mergeForwardSpecs("18080:80", parsed.ssh.localForwards) ==
            "18080:80,8080:localhost:80");
    REQUIRE(parsed.remainingArgs.back() == "host");
  }

  SECTION("-l is the username and --logdir stays the log directory") {
    auto parsed =
        parseArgv({"et", "-l", "alice", "--logdir", "/tmp/logs", "host"});
    REQUIRE(parsed.ssh.loginNameSet);
    REQUIRE(parsed.ssh.loginName == "alice");
    REQUIRE(parsed.remainingArgs[1] == "--logdir");
    REQUIRE(parsed.remainingArgs[2] == "/tmp/logs");
    REQUIRE(parsed.remainingArgs.back() == "host");
  }

  SECTION("-c is a cipher and the positional command is unchanged") {
    auto parsed = parseArgv({"et", "-c", "aes128-ctr", "host", "echo", "hi"});
    REQUIRE(parsed.ssh.cipherSet);
    REQUIRE(parsed.ssh.cipher == "aes128-ctr");
    EtArgvSplit split = splitEtArgvAtHost(parsed.remainingArgs);
    REQUIRE(split.commandOperands.size() == 2);
    REQUIRE(joinRemoteCommandOperands(split.commandOperands) == "echo hi");
    REQUIRE(remoteCommandConflictsWithNoCommand(false, "echo hi") == false);
  }

  SECTION("-v and -vv set verbosity without consuming the host") {
    auto once = parseArgv({"et", "-v", "host"});
    REQUIRE(once.ssh.verboseCount == 1);
    REQUIRE(once.remainingArgs.back() == "host");
    auto twice = parseArgv({"et", "-vv", "host"});
    REQUIRE(twice.ssh.verboseCount == 2);
    REQUIRE(twice.remainingArgs.back() == "host");
    auto clustered = parseArgv({"et", "-vvv", "host"});
    REQUIRE(clustered.ssh.verboseCount == 3);
  }

  SECTION("-x does not request killing other sessions") {
    auto parsed = parseArgv({"et", "-x", "host"});
    REQUIRE(parsed.ssh.disableX11);
    for (const auto& arg : parsed.remainingArgs) {
      REQUIRE(arg != "-x");
      REQUIRE(arg != "--kill-other-sessions");
    }
    REQUIRE(parsed.remainingArgs.back() == "host");
  }

  SECTION("-N with a remote command conflicts") {
    auto parsed = parseArgv({"et", "-N", "host", "echo", "hi"});
    REQUIRE(parsed.ssh.noRemoteCommand);
    EtArgvSplit split = splitEtArgvAtHost(parsed.remainingArgs);
    string command = joinRemoteCommandOperands(split.commandOperands);
    REQUIRE(command == "echo hi");
    REQUIRE(remoteCommandConflictsWithNoCommand(parsed.ssh.noRemoteCommand,
                                                command));
  }

  SECTION("later of -t and -T wins") {
    auto disable = parseArgv({"et", "-t", "-T", "host"});
    REQUIRE(disable.ssh.pty == PtyOverride::Disable);
    auto force = parseArgv({"et", "-T", "-t", "host"});
    REQUIRE(force.ssh.pty == PtyOverride::Force);
  }

  SECTION("--no-pty disables the pty like -T") {
    auto parsed = parseArgv({"et", "--no-pty", "host", "cmd"});
    REQUIRE(parsed.ssh.pty == PtyOverride::Disable);
    REQUIRE(remotePtyDisabled(parsed.ssh));
    EtArgvSplit split = splitEtArgvAtHost(parsed.remainingArgs);
    REQUIRE(split.clientArgs.size() == 3);
    REQUIRE(split.clientArgs[1] == "--no-pty");
    REQUIRE(split.clientArgs[2] == "host");
    REQUIRE(joinRemoteCommandOperands(split.commandOperands) == "cmd");

    vector<char*> clientArgv;
    for (auto& arg : split.clientArgs) {
      clientArgv.push_back(&arg[0]);
    }
    cxxopts::Options options("et", "et");
    options.add_options()("T,no-pty", "no pty")("host", "host",
                                                cxxopts::value<string>());
    options.parse_positional({"host"});
    auto result =
        options.parse(static_cast<int>(clientArgv.size()), clientArgv.data());
    REQUIRE(result.count("no-pty") == 1);
    REQUIRE(result["host"].as<string>() == "host");
  }

  SECTION("later of -t, -T, and --no-pty wins") {
    auto force = parseArgv({"et", "--no-pty", "-t", "host", "cmd"});
    REQUIRE(force.ssh.pty == PtyOverride::Force);
    auto disable = parseArgv({"et", "-t", "--no-pty", "host", "cmd"});
    REQUIRE(disable.ssh.pty == PtyOverride::Disable);
  }

  SECTION("-N accepts -T without a remote command") {
    auto noCommandNoPty =
        parseArgv({"et", "-NT", "-L", "8080:localhost:80", "host"});
    REQUIRE(noCommandNoPty.ssh.noRemoteCommand);
    REQUIRE(remoteCommandOptionsError(noCommandNoPty.ssh, "", false).empty());
    REQUIRE_FALSE(remotePtyDisabled(noCommandNoPty.ssh));
  }

  SECTION("-T still needs a command and -N still rejects one") {
    auto noPtyOnly = parseArgv({"et", "-T", "host"});
    REQUIRE_FALSE(remoteCommandOptionsError(noPtyOnly.ssh, "", false).empty());
    REQUIRE_FALSE(
        remoteCommandOptionsError(noPtyOnly.ssh, "cat", true).empty());
    REQUIRE(remoteCommandOptionsError(noPtyOnly.ssh, "cat", false).empty());

    auto noCommand = parseArgv({"et", "-N", "host", "echo", "hi"});
    REQUIRE_FALSE(
        remoteCommandOptionsError(noCommand.ssh, "echo hi", false).empty());
  }

  SECTION("only -p reaches the bootstrap ssh as a port") {
    auto withoutPort =
        parseArgv({"et", "-o", "Port=2200", "--ssh-option", "Port=22", "host"});
    REQUIRE_FALSE(bootstrapSshPort(withoutPort.ssh).set);

    auto withPort = parseArgv({"et", "-p", "2201", "host"});
    BootstrapSshPort explicitPort = bootstrapSshPort(withPort.ssh);
    REQUIRE(explicitPort.set);
    REQUIRE(explicitPort.port == 2201);
  }

  SECTION("later of -J and --jumphost wins") {
    auto jumpLast = parseArgv(
        {"et", "--jumphost", "first.example", "-J", "second.example", "host"});
    REQUIRE(resolveJumpHost(jumpLast.ssh, true, "first.example") ==
            "second.example");
    auto longLast = parseArgv(
        {"et", "-J", "first.example", "--jumphost", "second.example", "host"});
    REQUIRE(resolveJumpHost(longLast.ssh, true, "second.example") ==
            "second.example");
  }

  SECTION("attached -p and -L forms") {
    auto parsed = parseArgv({"et", "-p22", "-L8080:localhost:80", "host"});
    REQUIRE(parsed.ssh.sshPort == 22);
    REQUIRE(parsed.ssh.localForwards.size() == 1);
    REQUIRE(parsed.ssh.localForwards[0] == "8080:localhost:80");
    REQUIRE(parsed.remainingArgs.back() == "host");
  }

  SECTION("flags after the host stay in the remote command") {
    auto parsed = parseArgv({"et", "host", "sh", "-c", "echo hi"});
    REQUIRE_FALSE(parsed.ssh.cipherSet);
    EtArgvSplit split = splitEtArgvAtHost(parsed.remainingArgs);
    REQUIRE(joinRemoteCommandOperands(split.commandOperands) ==
            "sh -c echo hi");
  }
}

namespace {

ParsedConnect parseAfterPrePass(const MuxParseResult& parsed) {
  vector<const char*> argv;
  for (const auto& arg : parsed.remainingArgs) {
    argv.push_back(arg.c_str());
  }
  return parseEtConnectArgv(static_cast<int>(argv.size()), argv.data());
}

string hostAfterPrePass(const MuxParseResult& parsed) {
  auto connect = parseAfterPrePass(parsed);
  REQUIRE(connect.result.count("host") == 1);
  return connect.result["host"].as<string>();
}

}  // namespace

TEST_CASE("Unhandled OpenSSH letters do not end a short cluster",
          "[ClientArgParsing]") {
  SECTION("-Cp22 still sets the sshd port") {
    auto parsed = parseArgv({"et", "-Cp22", "host"});
    REQUIRE(parsed.ssh.sshPortSet);
    REQUIRE(parsed.ssh.sshPort == 22);
    REQUIRE(hostAfterPrePass(parsed) == "host");
  }

  SECTION("-qT still disables the pty") {
    auto parsed = parseArgv({"et", "-qT", "host", "cmd"});
    REQUIRE(parsed.ssh.pty == PtyOverride::Disable);
    auto connect = parseAfterPrePass(parsed);
    REQUIRE(connect.result["host"].as<string>() == "host");
    REQUIRE(commandFromParse(connect) == "cmd");
  }

  SECTION("-Cl consumes the next token as the login name") {
    auto parsed = parseArgv({"et", "-Cl", "alice", "host"});
    REQUIRE(parsed.ssh.loginNameSet);
    REQUIRE(parsed.ssh.loginName == "alice");
    REQUIRE(hostAfterPrePass(parsed) == "host");
  }

  SECTION("-CfNL applies every letter") {
    auto parsed = parseArgv({"et", "-CfNL", "8080:localhost:80", "host"});
    REQUIRE(parsed.ssh.background);
    REQUIRE(parsed.ssh.noRemoteCommand);
    REQUIRE(parsed.ssh.localForwards.size() == 1);
    REQUIRE(parsed.ssh.localForwards[0] == "8080:localhost:80");
    REQUIRE(hostAfterPrePass(parsed) == "host");
  }

  SECTION("-b consumes its bind address") {
    auto separate = parseArgv({"et", "-b", "10.0.0.1", "host"});
    REQUIRE(hostAfterPrePass(separate) == "host");
    auto attached = parseArgv({"et", "-b10.0.0.1", "host"});
    REQUIRE(hostAfterPrePass(attached) == "host");
    auto clustered = parseArgv({"et", "-Cb", "10.0.0.1", "host"});
    REQUIRE(hostAfterPrePass(clustered) == "host");
  }

  SECTION("letters unknown to ssh do not swallow later letters") {
    auto parsed = parseArgv({"et", "-Zp2201", "host"});
    REQUIRE(parsed.ssh.sshPortSet);
    REQUIRE(parsed.ssh.sshPort == 2201);
    REQUIRE(hostAfterPrePass(parsed) == "host");
  }
}

TEST_CASE("joinRemoteCommandOperands preserves argv words",
          "[ClientArgParsing]") {
  REQUIRE(joinRemoteCommandOperands({"echo", "hello", "world"}) ==
          "echo hello world");
  REQUIRE(joinRemoteCommandOperands({}) == "");
  REQUIRE(joinRemoteCommandOperands({"alone"}) == "alone");
}
