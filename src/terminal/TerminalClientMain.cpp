#include "TerminalClient.hpp"

#include "PsuedoTerminalConsole.hpp"
#include "ParseConfigFile.hpp"

using namespace et;
namespace google {}
namespace gflags {}
using namespace google;
using namespace gflags;

DEFINE_string(u, "", "username to login");
DEFINE_string(host, "localhost", "host to join");
DEFINE_int32(port, 2022, "port to connect on");
DEFINE_string(c, "", "Command to run immediately after connecting");
DEFINE_string(
    prefix, "",
    "Command prefix to launch etserver/etterminal on the server side");
DEFINE_string(t, "",
              "Array of source:destination ports or "
              "srcStart-srcEnd:dstStart-dstEnd (inclusive) port ranges (e.g. "
              "10080:80,10443:443, 10090-10092:8000-8002)");
DEFINE_string(rt, "",
              "Array of source:destination ports or "
              "srcStart-srcEnd:dstStart-dstEnd (inclusive) port ranges (e.g. "
              "10080:80,10443:443, 10090-10092:8000-8002)");
DEFINE_string(jumphost, "", "jumphost between localhost and destination");
DEFINE_int32(jport, 2022, "port to connect on jumphost");
DEFINE_bool(x, false, "flag to kill all old sessions belonging to the user");
DEFINE_int32(v, 0, "verbose level");
DEFINE_bool(logtostdout, false, "log to stdout");
DEFINE_bool(silent, false, "If enabled, disable logging");
DEFINE_bool(noratelimit, false,
            "There's 1024 lines/second limit, which can be "
            "disabled based on different use case.");

using namespace et;

int main(int argc, char** argv) {
  // Version string need to be set before GFLAGS parse arguments
  SetVersionString(string(ET_VERSION));

  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);

  // GFLAGS parse command line arguments
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_logtostdout) {
    defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
  } else {
    defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
    // Redirect std streams to a file
    LogHandler::stderrToFile("/tmp/etclient");
  }

  // silent Flag, since etclient doesn't read /etc/et.cfg file
  if (FLAGS_silent) {
    defaultConf.setGlobally(el::ConfigurationType::Enabled, "false");
  }

  LogHandler::setupLogFile(&defaultConf,
                           "/tmp/etclient-%datetime{%Y-%M-%d_%H_%m_%s}.log");

  el::Loggers::reconfigureLogger("default", defaultConf);
  // set thread name
  el::Helpers::setThreadName("client-main");

  // Install log rotation callback
  el::Helpers::installPreRollOutCallback(LogHandler::rolloutHandler);

  // Override -h & --help
  for (int i = 1; i < argc; i++) {
    string s(argv[i]);
    if (s == "-h" || s == "--help") {
      cout << "et (options) [user@]hostname[:port]\n"
              "Options:\n"
              "-h Basic usage\n"
              "-p Port for etserver to run on.  Default: 2022\n"
              "-u Username to connect to ssh & ET\n"
              "-v=9 verbose log files\n"
              "-c Initial command to execute upon connecting\n"
              "-prefix Command prefix to launch etserver/etterminal on the "
              "server side\n"
              "-t Map local to remote TCP port (TCP Tunneling)\n"
              "   example: et -t=\"18000:8000\" hostname maps localhost:18000\n"
              "-rt Map remote to local TCP port (TCP Reverse Tunneling)\n"
              "   example: et -rt=\"18000:8000\" hostname maps hostname:18000\n"
              "to localhost:8000\n"
              "-jumphost Jumphost between localhost and destination\n"
              "-jport Port to connect on jumphost\n"
              "-x Flag to kill all sessions belongs to the user\n"
              "-logtostdout Sent log message to stdout\n"
              "-silent Disable all logs\n"
              "-noratelimit Disable rate limit"
           << endl;
      exit(1);
    }
  }

  GOOGLE_PROTOBUF_VERIFY_VERSION;
  srand(1);

  // Parse command-line argument
  if (argc > 1) {
    string arg = string(argv[1]);
    if (arg.find('@') != string::npos) {
      int i = arg.find('@');
      FLAGS_u = arg.substr(0, i);
      arg = arg.substr(i + 1);
    }
    if (arg.find(':') != string::npos) {
      int i = arg.find(':');
      FLAGS_port = stoi(arg.substr(i + 1));
      arg = arg.substr(0, i);
    }
    FLAGS_host = arg;
  }

  Options options = {
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
      0      // gss_delegate_creds
  };

  char* home_dir = ssh_get_user_home_dir();
  string host_alias = FLAGS_host;
  ssh_options_set(&options, SSH_OPTIONS_HOST, FLAGS_host.c_str());
  // First parse user-specific ssh config, then system-wide config.
  parse_ssh_config_file(&options, string(home_dir) + USER_SSH_CONFIG_PATH);
  parse_ssh_config_file(&options, SYSTEM_SSH_CONFIG_PATH);
  LOG(INFO) << "Parsed ssh config file, connecting to " << options.host;
  FLAGS_host = string(options.host);

  // Parse username: cmdline > sshconfig > localuser
  if (FLAGS_u.empty()) {
    if (options.username) {
      FLAGS_u = string(options.username);
    } else {
      FLAGS_u = string(ssh_get_local_username());
    }
  }

  // Parse jumphost: cmd > sshconfig
  if (options.ProxyJump && FLAGS_jumphost.length() == 0) {
    string proxyjump = string(options.ProxyJump);
    size_t colonIndex = proxyjump.find(":");
    if (colonIndex != string::npos) {
      string userhostpair = proxyjump.substr(0, colonIndex);
      size_t atIndex = userhostpair.find("@");
      if (atIndex != string::npos) {
        FLAGS_jumphost = userhostpair.substr(atIndex + 1);
      }
    } else {
      FLAGS_jumphost = proxyjump;
    }
    LOG(INFO) << "ProxyJump found for dst in ssh config: " << proxyjump;
  }

  string idpasskeypair = SshSetupHandler::SetupSsh(
      FLAGS_u, FLAGS_host, host_alias, FLAGS_port, FLAGS_jumphost, FLAGS_jport,
      FLAGS_x, FLAGS_v, FLAGS_prefix, FLAGS_noratelimit);

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
  bool is_jumphost = false;
  if (!FLAGS_jumphost.empty()) {
    is_jumphost = true;
    FLAGS_host = FLAGS_jumphost;
    FLAGS_port = FLAGS_jport;
  }
  SocketEndpoint socketEndpoint =
      SocketEndpoint(FLAGS_host, FLAGS_port, is_jumphost);
  shared_ptr<SocketHandler> clientSocket(new TcpSocketHandler());
  shared_ptr<Console> console(new PsuedoTerminalConsole());

  TerminalClient terminalClient =
      TerminalClient(clientSocket, socketEndpoint, id, passkey, console);
  terminalClient.run(FLAGS_c, FLAGS_t, FLAGS_rt);

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();
  return 0;
}
