#include <ctime>
#include <cxxopts.hpp>
#include <iomanip>

#include "Headers.hpp"
#include "HostParsing.hpp"
#include "ParseConfigFile.hpp"
#include "PipeSocketHandler.hpp"
#include "PseudoTerminalConsole.hpp"
#include "SessionStore.hpp"
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

// Resolve a host alias via SSH config lookup
ResolvedSshConfig resolveSshConfigHost(const string& hostAlias) {
  ResolvedSshConfig result;
  result.hostname = hostAlias;  // Default to original if not resolved

  char* home_dir = ssh_get_user_home_dir();
  Options opts = {NULL, NULL, NULL, NULL, NULL, NULL, 0,    0, 0,
                  0,    0,    NULL, NULL, 0,    0,    NULL, {}};

  ssh_options_set(&opts, SSH_OPTIONS_HOST, hostAlias.c_str());
  parse_ssh_config_file(hostAlias.c_str(), &opts,
                        string(home_dir) + USER_SSH_CONFIG_PATH);
  parse_ssh_config_file(hostAlias.c_str(), &opts, SYSTEM_SSH_CONFIG_PATH);

  if (opts.host) {
    result.hostname = string(opts.host);
  }
  if (opts.username) {
    result.username = string(opts.username);
  }

  freeOptionsFields(&opts);
  free(home_dir);
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

  // Name of the session being started (empty when unnamed).  The saved file
  // is deleted when the server ends the session, and kept otherwise so the
  // session can be reattached later.
  string sessionName = "";
  // Set after run() returns: true when the server ended the session.
  bool sessionEndedByServer = false;
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
        "[OPTION...] [user@]host[:port]\n\n"
        "  Note that 'host' can be a hostname or ipv4 address with or without "
        "a port\n  or an ipv6 address. If the ipv6 address is abbreviated with "
        ":: then it must\n  be specified without a port (use -p,--port).");

    options.add_options()             //
        ("h,help", "Print help")      //
        ("version", "Print version")  //
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
        ("f,forward-ssh-agent", "Forward ssh-agent socket")     //
        ("ssh-socket", "The ssh-agent socket to forward",
         cxxopts::value<std::string>())  //
        ("telemetry",
         "Allow et to anonymously send errors to guide future improvements",
         cxxopts::value<bool>()->default_value("true"))  //
        ("name", "Name this session so it can be reattached later",
         cxxopts::value<std::string>())  //
        ("attach", "Reattach to a previously named session",
         cxxopts::value<std::string>())           //
        ("list", "List saved sessions and exit")  //
        ("serverfifo",
         "If set, communicate to etserver on the matching fifo name",
         cxxopts::value<std::string>()->default_value(""))  //
        ("ssh-option", "Options to pass down to `ssh -o`",
         cxxopts::value<std::vector<std::string>>());

    options.parse_positional({"host"});
    auto result = options.parse(argc, argv);

    if (result.count("help")) {
      CLOG(INFO, "stdout") << options.help({}) << endl;
      exit(0);
    }

    if (result.count("version")) {
      CLOG(INFO, "stdout") << "et version " << ET_VERSION << endl;
      exit(0);
    }

    if (result.count("list")) {
      // Local-only operation: no connection is made.
      CLOG(INFO, "stdout") << left << setw(24) << "NAME" << setw(24) << "HOST"
                           << setw(8) << "PORT" << "SAVED" << endl;
      for (const auto& session : listSessions()) {
        char saved[32];
        struct tm savedTm;
        // time_t is long on some platforms and long long on others; go
        // through an explicit time_t so this compiles everywhere.
        const time_t savedAt = static_cast<time_t>(session.savedAt);
        localtime_r(&savedAt, &savedTm);
        strftime(saved, sizeof(saved), "%Y-%m-%d %H:%M:%S", &savedTm);
        CLOG(INFO, "stdout")
            << left << setw(24) << session.name << setw(24) << session.host
            << setw(8) << session.port << saved << endl;
      }
      exit(0);
    }

    if (result.count("attach") &&
        (result.count("name") || result.count("host"))) {
      CLOG(INFO, "stdout") << "--attach takes a session name; it cannot be "
                              "combined with --name or a host"
                           << endl;
      exit(1);
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

    if (result.count("attach")) {
      // Reattach to a previously named session. The server-side session
      // (terminal + router entry) outlived the client, so skip ssh bootstrap
      // and connect straight to the saved endpoint with the saved id/key.
      const string attachName = result["attach"].as<string>();
      if (!isValidSessionName(attachName)) {
        CLOG(INFO, "stdout") << "Invalid session name: " << attachName << endl;
        exit(1);
      }
      optional<SessionInfo> session = loadSession(attachName);
      if (!session) {
        CLOG(INFO, "stdout")
            << "No saved session named '" << attachName << "'" << endl;
        for (const auto& s : listSessions()) {
          CLOG(INFO, "stdout") << "  " << s.name << " (" << s.host << ":"
                               << s.port << ")" << endl;
        }
        exit(1);
      }

      SocketEndpoint attachEndpoint;
      attachEndpoint.set_name(session->host);
      attachEndpoint.set_port(session->port);
      shared_ptr<SocketHandler> attachSocket(new TcpSocketHandler());
      shared_ptr<SocketHandler> attachPipeSocket(new PipeSocketHandler());

      if (!ping(attachEndpoint, attachSocket)) {
        CLOG(INFO, "stdout")
            << "Could not reach the ET server: " << attachEndpoint.name() << ":"
            << attachEndpoint.port() << endl;
        exit(1);
      }

      shared_ptr<Console> attachConsole;
      if (!result.count("N")) {
        attachConsole.reset(new PseudoTerminalConsole());
      }
      int attachKeepalive = extractSingleOptionWithDefault<int>(
          result, options, "keepalive", MAX_CLIENT_KEEP_ALIVE_DURATION);
      bool attachSessionEnded = false;
      try {
        TerminalClient attachClient(
            attachSocket, attachPipeSocket, attachEndpoint, session->id,
            session->passkey, attachConsole, /*jumphost=*/false,
            /*tunnels=*/"", /*reverseTunnels=*/"",
            /*forwardSshAgent=*/false, /*identityAgent=*/"", attachKeepalive,
            /*envVars=*/{}, /*maxConnectAttempts=*/15,
            /*exitOnConnectFailure=*/false);
        attachClient.run(
            result.count("command") ? result["command"].as<string>() : "",
            result.count("noexit"));
        attachSessionEnded = attachClient.sessionEndedByServer();
      } catch (const runtime_error& err) {
        if (string(err.what()) ==
            TerminalClient::INVALID_SESSION_CONNECT_ERROR) {
          deleteSession(attachName);
          CLOG(INFO, "stdout")
              << "Session '" << attachName << "' is no longer running on "
              << session->host << endl;
        } else {
          CLOG(INFO, "stdout") << "Could not attach to session '" << attachName
                               << "': " << err.what() << endl;
        }
        exit(1);
      }
      // Only drop the saved file when the server says the session is gone
      // (the remote shell ended).  A local exit — console closed, window
      // died — leaves the remote shell running, so the file stays.
      if (attachSessionEnded) {
        deleteSession(attachName);
      }
      exit(0);
    }
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
    if (host_arg.find('@') != string::npos) {
      int i = host_arg.find('@');
      username = host_arg.substr(0, i);
      host_arg = host_arg.substr(i + 1);
    }

    if (host_arg.find(':') != string::npos) {
      int colon_count = std::count(host_arg.begin(), host_arg.end(), ':');
      if (colon_count == 1) {
        // ipv4 or hostname with port specified
        int port_colon_pos = host_arg.rfind(':');
        destinationPort = stoi(host_arg.substr(port_colon_pos + 1));
        host_arg = host_arg.substr(0, port_colon_pos);
      } else {
        // maybe ipv6 (colon_count >= 2)
        if (host_arg.find("::") != string::npos) {
          // ipv6 with double colon zero abbreviation and no port
          // leave host_arg as is
        } else {
          if (colon_count == 7) {
            // ipv6, fully expanded, without port
          } else if (colon_count == 8) {
            // ipv6, fully expanded, with port
            int port_colon_pos = host_arg.rfind(':');
            destinationPort = stoi(host_arg.substr(port_colon_pos + 1));
            host_arg = host_arg.substr(0, port_colon_pos);
          } else {
            CLOG(INFO, "stdout") << "Invalid host positional arg: "
                                 << result["host"].as<std::string>() << endl;
            exit(1);
          }
        }
      }
    }
    destinationHost = host_arg;
    // host_alias is used for the initiating ssh call, if sshd runs on a port
    // other than 22, either configure your .ssh/config with an alias with an
    // overridden port or pass --ssh-option Port=<sshd_port>
    string host_alias = destinationHost;

    string jumphost =
        extractSingleOptionWithDefault<string>(result, options, "jumphost", "");
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

    {
      char* home_dir = ssh_get_user_home_dir();
      const char* host_from_command = destinationHost.c_str();
      ssh_options_set(&sshConfigOptions, SSH_OPTIONS_HOST,
                      destinationHost.c_str());
      // First parse user-specific ssh config, then system-wide config.
      parse_ssh_config_file(host_from_command, &sshConfigOptions,
                            string(home_dir) + USER_SSH_CONFIG_PATH);
      parse_ssh_config_file(host_from_command, &sshConfigOptions,
                            SYSTEM_SSH_CONFIG_PATH);
      if (sshConfigOptions.host) {
        LOG(INFO) << "Parsed ssh config file, connecting to "
                  << sshConfigOptions.host;
        destinationHost = string(sshConfigOptions.host);
      }
      free(home_dir);
    }

    // Every session gets a name so it can be reattached after the client
    // (or machine) restarts: explicit --name, or a host+timestamp default.
    if (result.count("name")) {
      sessionName = result["name"].as<string>();
      if (!isValidSessionName(sessionName)) {
        CLOG(INFO, "stdout") << "Invalid session name: " << sessionName << endl;
        exit(1);
      }
      if (loadSession(sessionName).has_value()) {
        CLOG(INFO, "stdout")
            << "session '" << sessionName << "' already exists; use --attach "
            << sessionName << " or choose another --name" << endl;
        exit(1);
      }
    } else {
      char ts[32];
      time_t now = time(NULL);
      struct tm localTm;
      localtime_r(&now, &localTm);
      strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &localTm);
      sessionName = destinationHost + "-" + ts;
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

    // Parse jumphost: cmd > sshconfig
    if (sshConfigOptions.ProxyJump && jumphost.length() == 0) {
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

      // Resolve jumphost alias to actual hostname via SSH config
      ResolvedSshConfig resolved = resolveSshConfigHost(parsed.host);
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
      if (jumphostUser.empty()) {
        char* localUsernamePtr = ssh_get_local_username();
        jumphostUser = string(localUsernamePtr);
        SAFE_FREE(localUsernamePtr);
      }

      // Reconstruct jumphost with resolved hostname for SSH -J flag
      jumphost = jumphostUser + "@" + resolved.hostname + parsed.portSuffix;

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
    if (!result.count("N")) {
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
    SshSetupHandler sshSetupHandler(subprocessUtils);
    pair<string, string> idpasskeypair = sshSetupHandler.SetupSsh(
        username, destinationHost, host_alias, destinationPort, jumphost,
        jServerFifo, result.count("x") > 0, result["verbose"].as<int>(),
        etterminal_path, serverFifo, ssh_options);

    TerminalClient terminalClient(
        clientSocket, clientPipeSocket, socketEndpoint, idpasskeypair.first,
        idpasskeypair.second, console, is_jumphost, tunnel_arg, r_tunnel_arg,
        forwardAgent, sshSocket, keepaliveDuration, sshConfigOptions.env_vars);

    // The connection is up: persist the session so a rebooted or killed
    // client can reattach with --attach.
    if (!sessionName.empty()) {
      SessionInfo sessionInfo;
      sessionInfo.name = sessionName;
      sessionInfo.host = socketEndpoint.name();
      sessionInfo.port = socketEndpoint.port();
      sessionInfo.id = idpasskeypair.first;
      sessionInfo.passkey = idpasskeypair.second;
      sessionInfo.savedAt = (int64_t)time(NULL);
      saveSession(sessionInfo);
    }
    terminalClient.run(
        result.count("command") ? result["command"].as<string>() : "",
        result.count("noexit"));
    sessionEndedByServer = terminalClient.sessionEndedByServer();
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

  // Drop the saved session file only when the server ended the session
  // (the remote shell exited; observed as INVALID_KEY).  Any other exit —
  // console EOF from a closed window, signal, crash — leaves the remote
  // shell running, so the file stays and the session can be reattached.
  if (!sessionName.empty() && sessionEndedByServer) {
    deleteSession(sessionName);
  }

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();

  return 0;
}
