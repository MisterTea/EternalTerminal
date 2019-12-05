#include "TerminalClient.hpp"

#include "ParseConfigFile.hpp"
#include "PipeSocketHandler.hpp"
#include "PsuedoTerminalConsole.hpp"

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

int main(int argc, char** argv) {
  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);

  // Parse command line arguments
  cxxopts::Options options("et", "Remote shell for the busy and impatient");
  try {
    options.positional_help("[user@]hostname[:port]").show_positional_help();
    options.allow_unrecognised_options();

    options.add_options()             //
        ("h,help", "Print help")      //
        ("version", "Print version")  //
        ("u,username", "Username")    //
        ("host", "Remote host name",
         cxxopts::value<std::string>())  //
        ("p,port", "Remote machine port",
         cxxopts::value<int>()->default_value("2022"))  //
        ("c,command", "Run command on connect",
         cxxopts::value<std::string>())  //
        ("prefix", "Add prefix when launching etterminal on server side",
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
        ("x,kill-other-sessions",
         "kill all old sessions belonging to the user")  //
        ("v,verbose", "Enable verbose logging",
         cxxopts::value<int>()->default_value("0"))          //
        ("logtostdout", "Write log to stdout")               //
        ("silent", "Disable logging")                        //
        ("N,no-terminal", "Do not create a terminal")        //
        ("f,forward-ssh-agent", "Forward ssh-agent socket")  //
        ("ssh-socket", "The ssh-agent socket to forward",
         cxxopts::value<std::string>())    //
        ("serverfifo",
         "If set, communicate to etserver on the matching fifo name",  //
         cxxopts::value<std::string>()->default_value(""))             //
        ;

    options.parse_positional({"host", "positional"});

    auto result = options.parse(argc, argv);
    if (result.count("help")) {
      cout << options.help({}) << endl;
      exit(0);
    }
    if (result.count("version")) {
      cout << "et version " << ET_VERSION << endl;
      exit(0);
    }

    if (result.count("logtostdout")) {
      defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
    } else {
      defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
      // Redirect std streams to a file
      LogHandler::stderrToFile("/tmp/etclient");
    }

    // silent Flag, since etclient doesn't read /etc/et.cfg file
    if (result.count("silent")) {
      defaultConf.setGlobally(el::ConfigurationType::Enabled, "false");
    }

    LogHandler::setupLogFile(&defaultConf,
                             "/tmp/etclient-%datetime{%Y-%M-%d_%H_%m_%s}.log");

    el::Loggers::reconfigureLogger("default", defaultConf);
    // set thread name
    el::Helpers::setThreadName("client-main");

    // Install log rotation callback
    el::Helpers::installPreRollOutCallback(LogHandler::rolloutHandler);

    GOOGLE_PROTOBUF_VERIFY_VERSION;
    srand(1);

    string username = "";
    if (result.count("username")) {
      username = result["username"].as<string>();
    }
    int destinationPort = result["port"].as<int>();
    string destinationHost;

    // Parse command-line argument
    if (!result.count("host")) {
      cout << "Missing host to connect to" << endl;
      cout << options.help({}) << endl;
      exit(0);
    }
    string arg = result["host"].as<std::string>();
    if (arg.find('@') != string::npos) {
      int i = arg.find('@');
      username = arg.substr(0, i);
      arg = arg.substr(i + 1);
    }
    if (arg.find(':') != string::npos) {
      int i = arg.find(':');
      destinationPort = stoi(arg.substr(i + 1));
      arg = arg.substr(0, i);
    }
    destinationHost = arg;

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

    char* home_dir = ssh_get_user_home_dir();
    string host_alias = destinationHost;
    const char *host_from_command = destinationHost.c_str();
    ssh_options_set(&sshConfigOptions, SSH_OPTIONS_HOST,
                    destinationHost.c_str());
    // First parse user-specific ssh config, then system-wide config.
    parse_ssh_config_file(host_from_command, &sshConfigOptions,
                          string(home_dir) + USER_SSH_CONFIG_PATH);
    parse_ssh_config_file(host_from_command, &sshConfigOptions, SYSTEM_SSH_CONFIG_PATH);
    LOG(INFO) << "Parsed ssh config file, connecting to "
              << sshConfigOptions.host;
    destinationHost = string(sshConfigOptions.host);

    // Parse username: cmdline > sshconfig > localuser
    if (username.empty()) {
      if (sshConfigOptions.username) {
        username = string(sshConfigOptions.username);
      } else {
        username = string(ssh_get_local_username());
      }
    }

    // Parse jumphost: cmd > sshconfig
    string jumphost =
        result.count("jumphost") ? result["jumphost"].as<string>() : "";
    if (sshConfigOptions.ProxyJump && jumphost.length() == 0) {
      string proxyjump = string(sshConfigOptions.ProxyJump);
      size_t colonIndex = proxyjump.find(":");
      if (colonIndex != string::npos) {
        string userhostpair = proxyjump.substr(0, colonIndex);
        size_t atIndex = userhostpair.find("@");
        if (atIndex != string::npos) {
          jumphost = userhostpair.substr(atIndex + 1);
        }
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
      socketEndpoint.set_name(jumphost);
      socketEndpoint.set_port(result["jport"].as<int>());
    } else {
      socketEndpoint.set_name(destinationHost);
      socketEndpoint.set_port(destinationPort);
    }
    shared_ptr<SocketHandler> clientSocket(new TcpSocketHandler());
    shared_ptr<SocketHandler> clientPipeSocket(new PipeSocketHandler());

    if (!ping(socketEndpoint, clientSocket)) {
      cout << "Could not reach the ET server: " << socketEndpoint.name() << ":"
           << socketEndpoint.port() << endl;
      exit(1);
    }

    int jport = result["jport"].as<int>();
    string serverFifo = "";
    if (result["serverfifo"].as<string>() != "") {
      serverFifo = result["serverfifo"].as<string>();
    }
    string idpasskeypair = SshSetupHandler::SetupSsh(
        username, destinationHost, host_alias, destinationPort, jumphost, jport,
        result.count("x") > 0, result["v"].as<int>(),
        result.count("prefix") ? result["prefix"].as<string>() : "",
        serverFifo);

    string id = "", passkey = "";
    // Trim whitespace
    idpasskeypair.erase(idpasskeypair.find_last_not_of(" \n\r\t") + 1);
    size_t slashIndex = idpasskeypair.find("/");
    if (slashIndex == string::npos) {
      LOG(FATAL) << "Invalid idPasskey id/key pair: " << idpasskeypair;
    } else {
      id = idpasskeypair.substr(0, slashIndex);
      passkey = idpasskeypair.substr(slashIndex + 1);
      LOG(INFO) << "ID PASSKEY: " << id << " " << passkey;
    }
    if (passkey.length() != 32) {
      LOG(FATAL) << "Invalid/missing passkey: " << passkey << " "
                 << passkey.length();
    }
    shared_ptr<Console> console;
    if (!result.count("N")) {
      console.reset(new PsuedoTerminalConsole());
    }

    TerminalClient terminalClient(
        clientSocket, clientPipeSocket, socketEndpoint, id, passkey, console,
        is_jumphost, result.count("t") ? result["t"].as<string>() : "",
        result.count("r") ? result["r"].as<string>() : "",
        (result.count("f") || sshConfigOptions.forward_agent),
        result.count("ssh-socket") ? result["ssh-socket"].as<string>() :
        sshConfigOptions.identity_agent ? string(sshConfigOptions.identity_agent) : "");
    terminalClient.run(result.count("command") ? result["command"].as<string>()
                                               : "");
  } catch (cxxopts::OptionException& oe) {
    cout << "Exception: " << oe.what() << "\n" << endl;
    cout << options.help({}) << endl;
    exit(1);
  }

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();
  return 0;
}
