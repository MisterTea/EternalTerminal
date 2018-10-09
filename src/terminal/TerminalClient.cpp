#include "ClientConnection.hpp"
#include "CryptoHandler.hpp"
#include "Headers.hpp"
#include "LogHandler.hpp"
#include "ParseConfigFile.hpp"
#include "PortForwardHandler.hpp"
#include "PortForwardSourceHandler.hpp"
#include "RawSocketUtils.hpp"
#include "ServerConnection.hpp"
#include "SshSetupHandler.hpp"
#include "TcpSocketHandler.hpp"

#include <errno.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>

#include "ETerminal.pb.h"

using namespace et;
namespace google {}
namespace gflags {}
using namespace google;
using namespace gflags;

const string SYSTEM_SSH_CONFIG_PATH = "/etc/ssh/ssh_config";
const string USER_SSH_CONFIG_PATH = "/.ssh/config";
const int KEEP_ALIVE_DURATION = 5;

shared_ptr<ClientConnection> globalClient;

termios terminal_backup;

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

shared_ptr<ClientConnection> createClient(string idpasskeypair) {
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

  InitialPayload payload;
  if (FLAGS_jumphost.length()) {
    payload.set_jumphost(true);
  }

  shared_ptr<SocketHandler> clientSocket(new TcpSocketHandler());
  shared_ptr<ClientConnection> client =
      shared_ptr<ClientConnection>(new ClientConnection(
          clientSocket, SocketEndpoint(FLAGS_host, FLAGS_port), id, passkey));

  int connectFailCount = 0;
  while (true) {
    try {
      client->connect();
      client->writeProto(payload);
    } catch (const runtime_error& err) {
      LOG(ERROR) << "Connecting to server failed: " << err.what();
      connectFailCount++;
      if (connectFailCount == 3) {
        LOG(INFO) << "Could not make initial connection to server";
        cout << "Could not make initial connection to " << FLAGS_host << ": "
             << err.what() << endl;
        exit(1);
      }
      continue;
    }
    break;
  }
  VLOG(1) << "Client created with id: " << client->getId();

  return client;
};

int firstWindowChangedCall = 1;
void handleWindowChanged(winsize* win) {
  winsize tmpwin;
  ioctl(1, TIOCGWINSZ, &tmpwin);
  if (firstWindowChangedCall || win->ws_row != tmpwin.ws_row ||
      win->ws_col != tmpwin.ws_col || win->ws_xpixel != tmpwin.ws_xpixel ||
      win->ws_ypixel != tmpwin.ws_ypixel) {
    firstWindowChangedCall = 0;
    *win = tmpwin;
    LOG(INFO) << "Window size changed: " << win->ws_row << " " << win->ws_col
              << " " << win->ws_xpixel << " " << win->ws_ypixel;
    TerminalInfo ti;
    ti.set_row(win->ws_row);
    ti.set_column(win->ws_col);
    ti.set_width(win->ws_xpixel);
    ti.set_height(win->ws_ypixel);
    string s(1, (char)et::PacketType::TERMINAL_INFO);
    globalClient->writeMessage(s);
    globalClient->writeProto(ti);
  }
}

vector<pair<int, int>> parseRangesToPairs(const string& input) {
  vector<pair<int, int>> pairs;
  auto j = split(input, ',');
  for (auto& pair : j) {
    vector<string> sourceDestination = split(pair, ':');
    try {
      if (sourceDestination[0].find('-') != string::npos &&
          sourceDestination[1].find('-') != string::npos) {
        vector<string> sourcePortRange = split(sourceDestination[0], '-');
        int sourcePortStart = stoi(sourcePortRange[0]);
        int sourcePortEnd = stoi(sourcePortRange[1]);

        vector<string> destinationPortRange = split(sourceDestination[1], '-');
        int destinationPortStart = stoi(destinationPortRange[0]);
        int destinationPortEnd = stoi(destinationPortRange[1]);

        if (sourcePortEnd - sourcePortStart !=
            destinationPortEnd - destinationPortStart) {
          LOG(FATAL) << "source/destination port range mismatch";
          exit(1);
        } else {
          int portRangeLength = sourcePortEnd - sourcePortStart + 1;
          for (int i = 0; i < portRangeLength; ++i) {
            pairs.push_back(
                make_pair(sourcePortStart + i, destinationPortStart + i));
          }
        }
      } else if (sourceDestination[0].find('-') != string::npos ||
                 sourceDestination[1].find('-') != string::npos) {
        LOG(FATAL) << "Invalid port range syntax: if source is range, "
                      "destination must be range";
      } else {
        int sourcePort = stoi(sourceDestination[0]);
        int destinationPort = stoi(sourceDestination[1]);
        pairs.push_back(make_pair(sourcePort, destinationPort));
      }
    } catch (const std::logic_error& lr) {
      LOG(FATAL) << "Logic error: " << lr.what();
      exit(1);
    }
  }
  return pairs;
}

int main(int argc, char** argv) {
  // Version string need to be set before GFLAGS parse arguments
  SetVersionString(string(ET_VERSION));

  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::SetupLogHandler(&argc, &argv);

  if (FLAGS_logtostdout) {
    defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
  } else {
    defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
  }

  // silent Flag, since etclient doesn't read /etc/et.cfg file
  if (FLAGS_silent) {
    defaultConf.setGlobally(el::ConfigurationType::Enabled, "false");
  }

  LogHandler::SetupLogFile(&defaultConf,
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
    LOG(INFO) << "ProxyJump found for dst in ssh config" << proxyjump;
  }

  string idpasskeypair = SshSetupHandler::SetupSsh(
      FLAGS_u, FLAGS_host, host_alias, FLAGS_port, FLAGS_jumphost, FLAGS_jport,
      FLAGS_x, FLAGS_v, FLAGS_prefix, FLAGS_noratelimit);

  time_t rawtime;
  struct tm* timeinfo;
  char buffer[80];

  time(&rawtime);
  timeinfo = localtime(&rawtime);

  strftime(buffer, sizeof(buffer), "%Y-%m-%d_%I-%M", timeinfo);
  string current_time(buffer);
  string errFilename = "/tmp/etclient_err_" + current_time;

  FILE* stderr_stream = freopen(errFilename.c_str(), "w+", stderr);
  setvbuf(stderr_stream, NULL, _IOLBF, BUFSIZ);  // set to line buffering

  if (!FLAGS_jumphost.empty()) {
    FLAGS_host = FLAGS_jumphost;
    FLAGS_port = FLAGS_jport;
  }
  globalClient = createClient(idpasskeypair);
  shared_ptr<TcpSocketHandler> socketHandler =
      static_pointer_cast<TcpSocketHandler>(globalClient->getSocketHandler());

  PortForwardHandler portForwardHandler(socketHandler);

  // Whether the TE should keep running.
  bool run = true;

// TE sends/receives data to/from the shell one char at a time.
#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

  time_t keepaliveTime = time(NULL) + KEEP_ALIVE_DURATION;
  bool waitingOnKeepalive = false;

  if (FLAGS_c.length()) {
    LOG(INFO) << "Got command: " << FLAGS_c;
    et::TerminalBuffer tb;
    tb.set_buffer(FLAGS_c + "; exit\n");

    char c = et::PacketType::TERMINAL_BUFFER;
    string headerString(1, c);
    globalClient->writeMessage(headerString);
    globalClient->writeProto(tb);
  }

  try {
    if (FLAGS_t.length()) {
      auto pairs = parseRangesToPairs(FLAGS_t);
      for (auto& pair : pairs) {
        PortForwardSourceRequest pfsr;
        pfsr.set_sourceport(pair.first);
        pfsr.set_destinationport(pair.second);
        auto pfsresponse = portForwardHandler.createSource(pfsr);
        if (pfsresponse.has_error()) {
          throw std::runtime_error(pfsresponse.error());
        }
      }
    }
    if (FLAGS_rt.length()) {
      auto pairs = parseRangesToPairs(FLAGS_rt);
      for (auto& pair : pairs) {
        char c = et::PacketType::PORT_FORWARD_SOURCE_REQUEST;
        string headerString(1, c);
        PortForwardSourceRequest pfsr;
        pfsr.set_sourceport(pair.first);
        pfsr.set_destinationport(pair.second);

        globalClient->writeMessage(headerString);
        globalClient->writeProto(pfsr);
      }
    }
  } catch (const std::runtime_error& ex) {
    cerr << "Error establishing port forward: " << ex.what() << endl;
    LOG(FATAL) << "Error establishing port forward: " << ex.what();
  }

  winsize win;
  ioctl(1, TIOCGWINSZ, &win);

  termios terminal_local;
  tcgetattr(0, &terminal_local);
  memcpy(&terminal_backup, &terminal_local, sizeof(struct termios));
  cfmakeraw(&terminal_local);
  tcsetattr(0, TCSANOW, &terminal_local);

  while (run && !globalClient->isShuttingDown()) {
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    int maxfd = STDIN_FILENO;
    FD_SET(STDIN_FILENO, &rfd);
    int clientFd = globalClient->getSocketFd();
    if (clientFd > 0) {
      FD_SET(clientFd, &rfd);
      maxfd = max(maxfd, clientFd);
    }
    // TODO: set port forward sockets as well for performance reasons.
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, NULL, NULL, &tv);

    try {
      // Check for data to send.
      if (FD_ISSET(STDIN_FILENO, &rfd)) {
        // Read from stdin and write to our client that will then send it to the
        // server.
        VLOG(4) << "Got data from stdin";
        int rc = read(STDIN_FILENO, b, BUF_SIZE);
        FATAL_FAIL(rc);
        if (rc > 0) {
          // VLOG(1) << "Sending byte: " << int(b) << " " << char(b) << " " <<
          // globalClient->getWriter()->getSequenceNumber();
          string s(b, rc);
          et::TerminalBuffer tb;
          tb.set_buffer(s);

          char c = et::PacketType::TERMINAL_BUFFER;
          string headerString(1, c);
          globalClient->writeMessage(headerString);
          globalClient->writeProto(tb);
          keepaliveTime = time(NULL) + KEEP_ALIVE_DURATION;
        }
      }

      if (clientFd > 0 && FD_ISSET(clientFd, &rfd)) {
        VLOG(4) << "Cliendfd is selected";
        while (globalClient->hasData()) {
          VLOG(4) << "GlobalClient has data";
          string packetTypeString;
          if (!globalClient->readMessage(&packetTypeString)) {
            break;
          }
          if (packetTypeString.length() != 1) {
            LOG(FATAL) << "Invalid packet header size: "
                       << packetTypeString.length();
          }
          char packetType = packetTypeString[0];
          if (packetType == et::PacketType::PORT_FORWARD_DATA ||
              packetType == et::PacketType::PORT_FORWARD_SOURCE_REQUEST ||
              packetType == et::PacketType::PORT_FORWARD_SOURCE_RESPONSE ||
              packetType == et::PacketType::PORT_FORWARD_DESTINATION_REQUEST ||
              packetType == et::PacketType::PORT_FORWARD_DESTINATION_RESPONSE) {
            keepaliveTime = time(NULL) + KEEP_ALIVE_DURATION;
            VLOG(4) << "Got PF packet type " << packetType;
            portForwardHandler.handlePacket(packetType, globalClient);
            continue;
          }
          switch (packetType) {
            case et::PacketType::TERMINAL_BUFFER: {
              VLOG(3) << "Got terminal buffer";
              // Read from the server and write to our fake terminal
              et::TerminalBuffer tb =
                  globalClient->readProto<et::TerminalBuffer>();
              const string& s = tb.buffer();
              // VLOG(5) << "Got message: " << s;
              // VLOG(1) << "Got byte: " << int(b) << " " << char(b) << " " <<
              // globalClient->getReader()->getSequenceNumber();
              keepaliveTime = time(NULL) + KEEP_ALIVE_DURATION;
              RawSocketUtils::writeAll(STDOUT_FILENO, &s[0], s.length());
              break;
            }
            case et::PacketType::KEEP_ALIVE:
              waitingOnKeepalive = false;
              // This will fill up log file quickly but is helpful for debugging
              // latency issues.
              LOG(INFO) << "Got a keepalive";
              break;
            default:
              LOG(FATAL) << "Unknown packet type: " << int(packetType);
          }
        }
      }

      if (clientFd > 0 && keepaliveTime < time(NULL)) {
        keepaliveTime = time(NULL) + KEEP_ALIVE_DURATION;
        if (waitingOnKeepalive) {
          LOG(INFO) << "Missed a keepalive, killing connection.";
          globalClient->closeSocket();
          waitingOnKeepalive = false;
        } else {
          LOG(INFO) << "Writing keepalive packet";
          string s(1, (char)et::PacketType::KEEP_ALIVE);
          globalClient->writeMessage(s);
          waitingOnKeepalive = true;
        }
      }
      if (clientFd < 0) {
        // We are disconnected, so stop waiting for keepalive.
        waitingOnKeepalive = false;
      }

      handleWindowChanged(&win);

      vector<PortForwardDestinationRequest> requests;
      vector<PortForwardData> dataToSend;
      portForwardHandler.update(&requests, &dataToSend);
      for (auto& pfr : requests) {
        char c = et::PacketType::PORT_FORWARD_DESTINATION_REQUEST;
        string headerString(1, c);
        globalClient->writeMessage(headerString);
        globalClient->writeProto(pfr);
        VLOG(4) << "send PF request";
        keepaliveTime = time(NULL) + KEEP_ALIVE_DURATION;
      }
      for (auto& pwd : dataToSend) {
        char c = PacketType::PORT_FORWARD_DATA;
        string headerString(1, c);
        globalClient->writeMessage(headerString);
        globalClient->writeProto(pwd);
        VLOG(4) << "send PF data";
        keepaliveTime = time(NULL) + KEEP_ALIVE_DURATION;
      }
    } catch (const runtime_error& re) {
      LOG(ERROR) << "Error: " << re.what();
      tcsetattr(0, TCSANOW, &terminal_backup);
      cout << "Connection closing because of error: " << re.what() << endl;
      run = false;
    }
  }
  globalClient.reset();
  LOG(INFO) << "Client derefernced";
  tcsetattr(0, TCSANOW, &terminal_backup);
  cout << "Session terminated" << endl;
  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();
  return 0;
}
