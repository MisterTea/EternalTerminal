#include <cxxopts.hpp>
#include <filesystem>
#include <fstream>
#include <limits>

#include "BinaryStdioConsole.hpp"
#include "ClientArgParsing.hpp"
#include "Headers.hpp"
#include "HostParsing.hpp"
#include "OpenSshLocalQueries.hpp"
#include "ParseConfigFile.hpp"
#include "PipeSocketHandler.hpp"
#include "PseudoTerminalConsole.hpp"
#include "SshSetupHandler.hpp"
#include "SubprocessUtils.hpp"
#include "TelemetryService.hpp"
#include "TerminalClient.hpp"
#include "TunnelUtils.hpp"
#include "WinsockContext.hpp"

using namespace et;

bool ping(SocketEndpoint socketEndpoint,
          shared_ptr<SocketHandler> clientSocketHandler) {
  VLOG(1) << "Connecting";
  int socketFd = clientSocketHandler->connect(socketEndpoint);
  if (socketFd == -1) {
    VLOG(1) << "Could not connect to host";
    return false;
  }
  clientSocketHandler->close(socketFd);
  return true;
}

void handleParseException(std::exception& e, cxxopts::Options& options) {
  CLOG(INFO, "stdout") << "Exception: " << e.what() << "\n" << endl;
  CLOG(INFO, "stdout") << options.help({}) << endl;
  exit(1);
}

template <class T, class DefaultT>
T extractSingleOptionWithDefault(const cxxopts::ParseResult& result,
                                 const cxxopts::Options& options,
                                 const string& name, DefaultT defaultValue) {
  auto count = result.count(name);
  if (count == 0) {
    return defaultValue;
  }
  if (count == 1) {
    return result[name].as<T>();
  }
  CLOG(INFO, "stdout") << "Value for " << name
                       << " must be specified only once\n";
  CLOG(INFO, "stdout") << options.help({}) << endl;
  exit(0);
}

// Resolved SSH config information for a host
struct ResolvedSshConfig {
  string hostname;  // Resolved HostName (or original if not an alias)
  string username;  // Username from SSH config (empty if not specified)
};

// Parse the selected SSH policy. An empty path preserves ET's legacy ambient
// behavior, "none" disables parsing, and every other value names the sole
// configuration file to read.
void parseSelectedSshConfig(const string& host, Options* options,
                            const string& sshConfigPath) {
  if (sshConfigPath == "none") {
    return;
  }
  if (!sshConfigPath.empty()) {
    parse_ssh_config_file(host.c_str(), options, sshConfigPath);
    return;
  }

  // Share seen[] across user then system so first-wins (user over system),
  // including explicit zero/"no" values that look unset on Options alone.
  int seen[SOC_END - SOC_UNSUPPORTED] = {0};
  char* homeDir = ssh_get_user_home_dir();
  if (homeDir != NULL) {
    parse_ssh_config_file(host.c_str(), options,
                          string(homeDir) + USER_SSH_CONFIG_PATH, seen);
    free(homeDir);
  }
  parse_ssh_config_file(host.c_str(), options, SYSTEM_SSH_CONFIG_PATH, seen);
}

// Resolve a host alias via SSH config lookup
ResolvedSshConfig resolveSshConfigHost(const string& hostAlias,
                                       const string& sshConfigPath) {
  ResolvedSshConfig result;
  result.hostname = hostAlias;  // Default to original if not resolved

  Options opts = {NULL, NULL, NULL, NULL, NULL, NULL, 0,    0, 0,
                  0,    0,    NULL, NULL, 0,    0,    NULL, {}};

  ssh_options_set(&opts, SSH_OPTIONS_HOST, hostAlias.c_str());
  parseSelectedSshConfig(hostAlias, &opts, sshConfigPath);

  if (opts.host) {
    result.hostname = string(opts.host);
  }
  if (opts.username) {
    result.username = string(opts.username);
  }

  freeOptionsFields(&opts);
  return result;
}

int main(int argc, char** argv) {
  WinsockContext context;
  string tmpDir = GetTempDirectory();

  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);
  LogHandler::setupStdoutLogger();

  et::HandleTerminate();

  // Override easylogging handler for sigint
  ::signal(SIGINT, et::InterruptSignalHandler);

  Options sshConfigOptions = {
      NULL,  // username
      NULL,  // host
      NULL,  // sshdir
      NULL,  // knownhosts
      NULL,  // ProxyCommand
      NULL,  // ProxyJump
      0,     // timeout
      0,     // port
      0,     // StrictHostKeyChecking
      0,     // ssh2
      0,     // ssh1
      NULL,  // gss_server_identity
      NULL,  // gss_client_identity
      0,     // gss_delegate_creds
      0,     // forward_agent
      NULL,  // identity_agent
      {}     // local_forwards (empty vector)
  };

  // Parse command line arguments
  cxxopts::Options options("et", "Remote shell for the busy and impatient");
  try {
    options.allow_unrecognised_options();
    options.positional_help("");
    options.custom_help(
        "[OPTION...] [user@]host[:port] [command...]\n\n"
        "  Note that 'host' can be a hostname or ipv4 address with or without "
        "a port\n  or an ipv6 address. If the ipv6 address is abbreviated with "
        ":: then it must\n  be specified without a port (use -p,--port).\n"
        "  A positional command after the host is equivalent to -c/--command "
        "(ssh-style).");

    options.add_options()             //
        ("h,help", "Print help")      //
        ("version", "Print version")  //
        ("V",
         "Print an OpenSSH-compatible version line and exit")  //
        ("G",
         "Print resolved OpenSSH-style configuration for the destination and "
         "exit without connecting")  //
        ("u,username", "Username",
         cxxopts::value<std::string>())  //
        ("host", "Remote host name",
         cxxopts::value<std::string>())  //
        ("p,port", "Remote machine etserver port",
         cxxopts::value<int>()->default_value("2022"))  //
        ("c,command", "Run command on connect and exit after command is run",
         cxxopts::value<std::string>())  //
        ("e,noexit",
         "Used together with -c to not exit after command is run")  //
        ("terminal-path",
         "Path to etterminal on server side. "
         "Use if etterminal is not on the system path.",
         cxxopts::value<std::string>())  //
        ("t,tunnel",
         "Tunnel: Array of source:destination ports or "
         "srcStart-srcEnd:dstStart-dstEnd (inclusive) port ranges (e.g. "
         "10080:80,10443:443, 10090-10092:8000-8002), ssh-style -L/-R "
         "argument, or Unix socket paths (e.g. "
         "/tmp/local.sock:/tmp/remote.sock, 8080:/tmp/remote.sock, "
         "/tmp/local.sock:8080). Defaults to localhost for bind address "
         "unless ssh-style tunnel argument is used.",
         cxxopts::value<std::string>())  //
        ("r,reversetunnel",
         "Reverse Tunnel: Same syntax as -t/--tunnel but reversed.",
         cxxopts::value<std::string>())  //
        ("jumphost", "jumphost between localhost and destination",
         cxxopts::value<std::string>())  //
        ("jport", "Jumphost machine port",
         cxxopts::value<int>()->default_value("2022"))  //
        ("jserverfifo",
         "If set, communicate to jumphost on the matching fifo name",
         cxxopts::value<string>()->default_value(""))  //
        ("x,kill-other-sessions",
         "kill all old sessions belonging to the user")  //
        ("close-on-hangup",
         "terminate the remote session when this terminal receives SIGHUP or "
         "closes")  //
        ("disconnect-timeout",
         "Minutes a disconnected etterminal may stay alive before etserver "
         "closes it for this session. 0 means no timeout. Overrides the "
         "etserver global when set.",
         cxxopts::value<int>())  //
        ("macserver",
         "Set when connecting to an macOS server.  Sets "
         "--terminal-path=/usr/local/bin/etterminal")  //
        ("v,verbose", "Enable verbose logging",
         cxxopts::value<int>()->default_value("0"))  //
        ("k,keepalive", "Client keepalive duration in seconds",
         cxxopts::value<int>())  //
        ("l,logdir", "Base directory for log files.",
         cxxopts::value<std::string>()->default_value(tmpDir))  //
        ("logtostdout", "Write log to stdout")                  //
        ("silent", "Disable logging")                           //
        ("N,no-terminal", "Do not create a terminal")           //
        ("D,dynamic",
         "Dynamic application-level port forwarding: listen on "
         "[bind_address:]port and accept SOCKS4/SOCKS5 connections that "
         "choose a remote TCP or Unix destination after connect (ssh -D). "
         "May be specified multiple times.",
         cxxopts::value<std::vector<std::string>>())  //
        ("W,stdio-forward",
         "Forward client stdio to host:port (or a Unix socket path) over the "
         "secure channel without a remote shell (ssh -W). Implies no local "
         "terminal.",
         cxxopts::value<std::string>())  //
        ("T,no-pty",
         "Run -c command on pipes instead of a pty (binary stdio, "
         "separate stderr, no shell injection)")             //
        ("f,forward-ssh-agent", "Forward ssh-agent socket")  //
        ("ssh-socket", "The ssh-agent socket to forward",
         cxxopts::value<std::string>())  //
        ("F,ssh-config",
         "Read only this absolute SSH configuration file (or 'none')",
         cxxopts::value<std::string>())  //
        ("no-ssh-config",
         "Do not read user or system SSH configuration for the destination "
         "or jumphost")  //
        ("telemetry",
         "Allow et to anonymously send errors to guide future improvements",
         cxxopts::value<bool>()->default_value("true"))  //
        ("serverfifo",
         "If set, communicate to etserver on the matching fifo name",
         cxxopts::value<std::string>()->default_value(""))  //
        ("ssh-option", "Options to pass down to `ssh -o`",
         cxxopts::value<std::vector<std::string>>())  //
        ("o",
         "OpenSSH-style session option applied to the resolved config "
         "(e.g. -o ConnectTimeout=10). Distinct from --ssh-option.",
         cxxopts::value<std::vector<std::string>>());

    options.parse_positional({"host"});
    vector<string> rawArgs;
    rawArgs.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
      rawArgs.emplace_back(argv[i]);
    }
    EtArgvSplit argvSplit = splitEtArgvAtHost(rawArgs);
    vector<char*> clientArgv;
    clientArgv.reserve(argvSplit.clientArgs.size());
    for (auto& arg : argvSplit.clientArgs) {
      clientArgv.push_back(&arg[0]);
    }
    auto result =
        options.parse(static_cast<int>(clientArgv.size()), clientArgv.data());
    TerminalClient::configureCloseOnHangup(result.count("close-on-hangup"));
    if (result.count("close-on-hangup")) {
#ifdef WIN32
      SetConsoleCtrlHandler(TerminalClient::consoleCtrlHandler, TRUE);
#else
      ::signal(SIGHUP, TerminalClient::requestHangupClose);
#endif
    }

    if (result.count("help")) {
      CLOG(INFO, "stdout") << options.help({}) << endl;
      exit(0);
    }

    if (result.count("version")) {
      CLOG(INFO, "stdout") << "et version " << ET_VERSION << endl;
      exit(0);
    }

    if (result.count("V")) {
      CLOG(INFO, "stdout") << openSshCompatibilityVersionLine() << endl;
      exit(0);
    }

    el::Loggers::setVerboseLevel(result["verbose"].as<int>());

    // silent Flag, since etclient doesn't read /etc/et.cfg file
    if (result.count("silent")) {
      defaultConf.setGlobally(el::ConfigurationType::Enabled, "false");
    }

    LogHandler::setupLogFiles(&defaultConf, result["logdir"].as<string>(),
                              "etclient", result.count("logtostdout"),
                              !result.count("logtostdout"));

    el::Loggers::reconfigureLogger("default", defaultConf);
    // set thread name
    el::Helpers::setThreadName("client-main");

    // Install log rotation callback
    el::Helpers::installPreRollOutCallback(LogHandler::rolloutHandler);

    GOOGLE_PROTOBUF_VERIFY_VERSION;
    srand(1);

    TelemetryService::create(result["telemetry"].as<bool>(),
                             tmpDir + "/.sentry-native-et", "Client");

    string username = "";
    if (result.count("username")) {
      username = result["username"].as<string>();
    }
    int destinationPort = result["port"].as<int>();
    string destinationHost;

    // Parse command-line argument
    if (!result.count("host")) {
      CLOG(INFO, "stdout") << "Missing host to connect to" << endl;
      CLOG(INFO, "stdout") << options.help({}) << endl;
      exit(0);
    }
    string host_arg = result["host"].as<std::string>();
    ParsedEtDestination parsedDestination;
    try {
      parsedDestination = parseEtDestinationHost(host_arg);
    } catch (const std::invalid_argument&) {
      CLOG(INFO, "stdout") << "Invalid host positional arg: " << host_arg
                           << endl;
      exit(1);
    }
    if (!parsedDestination.username.empty()) {
      username = parsedDestination.username;
    }
    if (parsedDestination.hasExplicitPort) {
      destinationPort = parsedDestination.port;
    }
    destinationHost = parsedDestination.host;
    // host_alias is used for the initiating ssh call, if sshd runs on a port
    // other than 22, either configure your .ssh/config with an alias with an
    // overridden port or pass --ssh-option Port=<sshd_port>
    string host_alias = destinationHost;

    const bool jumphostSpecified = result.count("jumphost") > 0;
    string jumphost =
        extractSingleOptionWithDefault<string>(result, options, "jumphost", "");
    if (strcasecmp(jumphost.c_str(), "none") == 0) {
      jumphost.clear();
    }
    if (result.count("ssh-config") && result.count("no-ssh-config")) {
      CLOG(INFO, "stdout")
          << "--ssh-config and --no-ssh-config are mutually exclusive" << endl;
      exit(1);
    }
    string sshConfigPath;
    if (result.count("ssh-config")) {
      sshConfigPath = result["ssh-config"].as<string>();
      if (sshConfigPath != "none" &&
          !std::filesystem::path(sshConfigPath).is_absolute()) {
        CLOG(INFO, "stdout")
            << "--ssh-config must be an absolute path or 'none': "
            << sshConfigPath << endl;
        exit(1);
      }
      if (sshConfigPath != "none" &&
          !SshSetupHandler::IsSshConfigPathSafeForProxyJump(sshConfigPath)) {
        CLOG(INFO, "stdout")
            << "--ssh-config must contain only ASCII letters, digits, '/', "
#ifdef WIN32
               "'\\\\', ':', "
#endif
               "'.', '_', and '-'; OpenSSH does not quote this path when "
               "propagating it through ProxyJump"
            << endl;
        exit(1);
      }
      if (sshConfigPath != "none") {
        std::error_code configError;
        std::filesystem::file_status configStatus =
            std::filesystem::symlink_status(sshConfigPath, configError);
        bool regularFile = std::filesystem::is_regular_file(configStatus);
        bool readableFile = false;
        if (!configError && regularFile) {
          std::ifstream configFile(sshConfigPath);
          readableFile = configFile.good();
        }
        if (!readableFile) {
          CLOG(INFO, "stdout")
              << "--ssh-config must name a readable, non-symlink regular file"
              << endl;
          exit(1);
        }
      }
    } else if (result.count("no-ssh-config")) {
      sshConfigPath = "none";
    }
    bool noSshConfig = sshConfigPath == "none";
    int keepaliveDuration = extractSingleOptionWithDefault<int>(
        result, options, "keepalive", MAX_CLIENT_KEEP_ALIVE_DURATION);
    if (keepaliveDuration < 1 ||
        keepaliveDuration > MAX_CLIENT_KEEP_ALIVE_DURATION) {
      CLOG(INFO, "stdout") << "Keep-alive duration must between 1 and "
                           << MAX_CLIENT_KEEP_ALIVE_DURATION << " seconds"
                           << endl;
      CLOG(INFO, "stdout") << options.help({}) << endl;
      exit(0);
    }

    optional<int> disconnectTimeoutMinutes;
    if (result.count("disconnect-timeout")) {
      int minutes = result["disconnect-timeout"].as<int>();
      if (minutes < 0) {
        CLOG(INFO, "stdout")
            << "--disconnect-timeout must be a non-negative number of minutes"
            << endl;
        CLOG(INFO, "stdout") << options.help({}) << endl;
        exit(1);
      }
      if (minutes > std::numeric_limits<int>::max() / 60) {
        CLOG(INFO, "stdout") << "--disconnect-timeout is too large" << endl;
        CLOG(INFO, "stdout") << options.help({}) << endl;
        exit(1);
      }
      disconnectTimeoutMinutes = minutes;
    }

    if (!noSshConfig) {
      ssh_options_set(&sshConfigOptions, SSH_OPTIONS_HOST,
                      destinationHost.c_str());
      parseSelectedSshConfig(destinationHost, &sshConfigOptions, sshConfigPath);
      if (sshConfigOptions.host) {
        LOG(INFO) << "Parsed ssh config file, connecting to "
                  << sshConfigOptions.host;
        destinationHost = string(sshConfigOptions.host);
      }
    }

    // Parse username: cmdline > sshconfig > localuser
    if (username.empty()) {
      if (sshConfigOptions.username) {
        username = string(sshConfigOptions.username);
      } else {
        char* usernamePtr = ssh_get_local_username();
        username = string(usernamePtr);
        SAFE_FREE(usernamePtr);
      }
    }

    // Apply OpenSSH-style -o session options after config resolution so they
    // override file values. --ssh-option remains a separate bootstrap-ssh path.
    if (result.count("o")) {
      for (const auto& sessionOption : result["o"].as<std::vector<string>>()) {
        if (!applySessionOption(&sshConfigOptions, sessionOption)) {
          CLOG(INFO, "stdout")
              << "Invalid -o option: " << sessionOption << endl;
          exit(1);
        }
        string key = sessionOption;
        size_t sep = key.find('=');
        if (sep == string::npos) {
          sep = key.find(' ');
        }
        if (sep != string::npos) {
          key = key.substr(0, sep);
        }
        key = lowercaseAscii(trimAsciiBlanks(std::move(key)));
        if (key == "hostname" && sshConfigOptions.host) {
          destinationHost = string(sshConfigOptions.host);
        } else if (key == "user" && sshConfigOptions.username) {
          username = string(sshConfigOptions.username);
        }
      }
    }

    if (result.count("G")) {
      CLOG(INFO, "stdout") << formatOpenSshResolvedConfig(
          host_alias, destinationHost, username, sshConfigOptions);
      exit(0);
    }

    // Parse jumphost: cmd > sshconfig
    if (!jumphostSpecified && sshConfigOptions.ProxyJump &&
        strcasecmp(sshConfigOptions.ProxyJump, "none") != 0 &&
        jumphost.length() == 0) {
      string proxyjump = string(sshConfigOptions.ProxyJump);
      // Keep full ProxyJump value including SSH port for ssh -J command
      jumphost = proxyjump;
      LOG(INFO) << "ProxyJump found for dst in ssh config: " << proxyjump;
    }

    bool is_jumphost = false;
    SocketEndpoint socketEndpoint;
    if (!jumphost.empty()) {
      is_jumphost = true;
      LOG(INFO) << "Setting port to jumphost port";

      // Parse [user@]host[:sshport] format
      ParsedHostString parsed = parseHostString(jumphost);

      // Resolve jumphost aliases only when SSH configuration is enabled.
      // In --no-ssh-config mode, keep the command-line host and user exact.
      ResolvedSshConfig resolved =
          resolveSshConfigHost(parsed.host, sshConfigPath);
      if (resolved.hostname != parsed.host) {
        LOG(INFO) << "Resolved jumphost alias '" << parsed.host
                  << "' to hostname: " << resolved.hostname;
      }

      // Determine username: command-line > SSH config > local user
      string jumphostUser = parsed.user;
      if (jumphostUser.empty() && !resolved.username.empty()) {
        jumphostUser = resolved.username;
        LOG(INFO) << "Using jumphost username from SSH config: "
                  << jumphostUser;
      }
      if (jumphostUser.empty() && !noSshConfig) {
        char* localUsernamePtr = ssh_get_local_username();
        jumphostUser = string(localUsernamePtr);
        SAFE_FREE(localUsernamePtr);
      }

      // Reconstruct jumphost with resolved hostname for SSH -J flag
      jumphost = (jumphostUser.empty() ? "" : jumphostUser + "@") +
                 resolved.hostname + parsed.portSuffix;

      socketEndpoint.set_name(resolved.hostname);
      socketEndpoint.set_port(result["jport"].as<int>());
    } else {
      socketEndpoint.set_name(destinationHost);
      socketEndpoint.set_port(destinationPort);
    }
    shared_ptr<SocketHandler> clientSocket(new TcpSocketHandler());
    shared_ptr<SocketHandler> clientPipeSocket(new PipeSocketHandler());

    if (!ping(socketEndpoint, clientSocket)) {
      CLOG(INFO, "stdout") << "Could not reach the ET server: "
                           << socketEndpoint.name() << ":"
                           << socketEndpoint.port() << endl;
      exit(1);
    }

    string jServerFifo = "";
    if (result["jserverfifo"].as<string>() != "") {
      jServerFifo = result["jserverfifo"].as<string>();
    }

    string serverFifo = "";
    if (result["serverfifo"].as<string>() != "") {
      serverFifo = result["serverfifo"].as<string>();
    }
    std::vector<string> ssh_options;
    if (result.count("ssh-option")) {
      ssh_options = result["ssh-option"].as<std::vector<string>>();
    }
    string etterminal_path = "";
    if (result.count("macserver") > 0) {
      etterminal_path = "/usr/local/bin/etterminal";
    }
    if (result.count("terminal-path")) {
      etterminal_path = result["terminal-path"].as<string>();
    }

    shared_ptr<Console> console;
    string stdioForward = extractSingleOptionWithDefault<string>(
        result, options, "stdio-forward", "");
    const bool noPty = result.count("T") > 0;
    string command = resolveRemoteCommand(
        argvSplit.commandOperands, result.count("command") > 0,
        result.count("command") ? result["command"].as<string>() : "");
    if (noPty && command.empty()) {
      CLOG(INFO, "stdout") << "-T/--no-pty requires -c/--command" << endl;
      CLOG(INFO, "stdout") << options.help({}) << endl;
      exit(1);
    }
    if (noPty && !stdioForward.empty()) {
      CLOG(INFO, "stdout") << "-W/--stdio-forward cannot be combined with "
                              "-T/--no-pty"
                           << endl;
      exit(1);
    }
    if (!stdioForward.empty() || result.count("N")) {
      // -W ties stdio to a remote destination; do not attach a local shell.
    } else if (noPty) {
      console.reset(new BinaryStdioConsole());
    } else {
      console.reset(new PseudoTerminalConsole());
    }

    bool forwardAgent = result.count("f") > 0;
    string sshSocket = "";
#ifndef WIN32
    if (sshConfigOptions.identity_agent) {
      sshSocket = string(sshConfigOptions.identity_agent);
    }
    forwardAgent |= sshConfigOptions.forward_agent;
#endif
    if (result.count("ssh-socket")) {
      sshSocket = result["ssh-socket"].as<string>();
    }
    TelemetryService::get()->logToDatadog("Session Started", el::Level::Info,
                                          __FILE__, __LINE__);
    string tunnel_arg =
        extractSingleOptionWithDefault<string>(result, options, "tunnel", "");
    string r_tunnel_arg = extractSingleOptionWithDefault<string>(
        result, options, "reversetunnel", "");
    vector<string> dynamicForwards;
    if (result.count("dynamic")) {
      dynamicForwards = result["dynamic"].as<vector<string>>();
    }

    for (const auto& localForward : sshConfigOptions.local_forwards) {
      string tunnelEntry =
          to_string(localForward.first) + ":" + to_string(localForward.second);
      LOG(INFO) << "Adding tunnel from SSH config LocalForward: "
                << tunnelEntry;
      if (tunnel_arg.empty()) {
        tunnel_arg = tunnelEntry;
      } else {
        tunnel_arg += "," + tunnelEntry;
      }
    }

    auto subprocessUtils = make_shared<SubprocessUtils>();
    SshSetupHandler sshSetupHandler(subprocessUtils, sshConfigPath);
    sshSetupHandler.setDisplayLoginOutput(console != nullptr);
    pair<string, string> idpasskeypair = sshSetupHandler.SetupSsh(
        username, destinationHost, host_alias, destinationPort, jumphost,
        jServerFifo, result.count("x") > 0, result["verbose"].as<int>(),
        etterminal_path, serverFifo, ssh_options);

    TerminalClient terminalClient(
        clientSocket, clientPipeSocket, socketEndpoint, idpasskeypair.first,
        idpasskeypair.second, console, is_jumphost, tunnel_arg, r_tunnel_arg,
        forwardAgent, sshSocket, keepaliveDuration, sshConfigOptions.env_vars,
        noPty, command, dynamicForwards, stdioForward,
        disconnectTimeoutMinutes);
    const int remoteExitStatus =
        terminalClient.run(command, result.count("noexit"));

    // Clean up ssh config options
    freeOptionsFields(&sshConfigOptions);

#ifdef WIN32
    WSACleanup();
#endif

    TelemetryService::get()->shutdown();
    TelemetryService::destroy();

    // Uninstall log rotation callback
    el::Helpers::uninstallPreRollOutCallback();

    return remoteExitStatus;
  } catch (TunnelParseException& tpe) {
    handleParseException(tpe, options);
  } catch (cxxopts::exceptions::exception& oe) {
    handleParseException(oe, options);
  }

  // Clean up ssh config options
  freeOptionsFields(&sshConfigOptions);

#ifdef WIN32
  WSACleanup();
#endif

  TelemetryService::get()->shutdown();
  TelemetryService::destroy();

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();

  return 0;
}
