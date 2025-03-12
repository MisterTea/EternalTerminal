#include <cxxopts.hpp>

#include "Headers.hpp"
#include "ParseConfigFile.hpp"
#include "PipeSocketHandler.hpp"
#include "PsuedoTerminalConsole.hpp"
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
      NULL   // identity_agent
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
        ("u,username", "Username")    //
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
         "10080:80,10443:443, 10090-10092:8000-8002)",
         cxxopts::value<std::string>())  //
        ("r,reversetunnel",
         "Reverse Tunnel: Array of source:destination ports or "
         "srcStart-srcEnd:dstStart-dstEnd (inclusive) port ranges",
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
      size_t colonIndex = proxyjump.find(":");
      if (colonIndex != string::npos) {
        jumphost = proxyjump.substr(0, colonIndex);
      } else {
        jumphost = proxyjump;
      }
      LOG(INFO) << "ProxyJump found for dst in ssh config: " << proxyjump;
    }

    bool is_jumphost = false;
    SocketEndpoint socketEndpoint;
    if (!jumphost.empty()) {
      is_jumphost = true;
      LOG(INFO) << "Setting port to jumphost port";
      size_t atIndex = jumphost.find("@");
      if (atIndex != string::npos) {
        socketEndpoint.set_name(jumphost.substr(atIndex + 1));
      } else {
        socketEndpoint.set_name(jumphost);
        jumphost = username + "@" + jumphost;
      }
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
    string idpasskeypair = SshSetupHandler::SetupSsh(
        username, destinationHost, host_alias, destinationPort, jumphost,
        jServerFifo, result.count("x") > 0, result["verbose"].as<int>(),
        etterminal_path, serverFifo, ssh_options);

    string id = "", passkey = "";
    // Trim whitespace
    idpasskeypair.erase(idpasskeypair.find_last_not_of(" \n\r\t") + 1);
    size_t slashIndex = idpasskeypair.find("/");
    if (slashIndex == string::npos) {
      STFATAL << "Invalid idPasskey id/key pair: " << idpasskeypair;
    } else {
      id = idpasskeypair.substr(0, slashIndex);
      passkey = idpasskeypair.substr(slashIndex + 1);
    }
    if (passkey.length() != 32) {
      STFATAL << "Invalid/missing passkey: " << passkey << " "
              << passkey.length();
    }
    shared_ptr<Console> console;
    if (!result.count("N")) {
      console.reset(new PsuedoTerminalConsole());
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
    TerminalClient terminalClient(clientSocket, clientPipeSocket,
                                  socketEndpoint, id, passkey, console,
                                  is_jumphost, tunnel_arg, r_tunnel_arg,
                                  forwardAgent, sshSocket, keepaliveDuration);
    terminalClient.run(
        result.count("command") ? result["command"].as<string>() : "",
        result.count("noexit"));
  } catch (TunnelParseException& tpe) {
    handleParseException(tpe, options);
  } catch (cxxopts::exceptions::exception& oe) {
    handleParseException(oe, options);
  }

  // Clean up ssh config options
  SAFE_FREE(sshConfigOptions.username);
  SAFE_FREE(sshConfigOptions.host);
  SAFE_FREE(sshConfigOptions.sshdir);
  SAFE_FREE(sshConfigOptions.knownhosts);
  SAFE_FREE(sshConfigOptions.ProxyCommand);
  SAFE_FREE(sshConfigOptions.ProxyJump);
  SAFE_FREE(sshConfigOptions.gss_server_identity);
  SAFE_FREE(sshConfigOptions.gss_client_identity);
  SAFE_FREE(sshConfigOptions.identity_agent);

#ifdef WIN32
  WSACleanup();
#endif

  TelemetryService::get()->shutdown();
  TelemetryService::destroy();

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();

  return 0;
}
