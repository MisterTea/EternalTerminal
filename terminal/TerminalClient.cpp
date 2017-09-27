#include "ClientConnection.hpp"
#include "CryptoHandler.hpp"
#include "FlakyFakeSocketHandler.hpp"
#include "Headers.hpp"
#include "PortForwardClientListener.hpp"
#include "PortForwardClientRouter.hpp"
#include "ServerConnection.hpp"
#include "SocketUtils.hpp"
#include "UnixSocketHandler.hpp"
#include "ParseConfigFile.hpp"

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

shared_ptr<ClientConnection> globalClient;

termios terminal_backup;

DEFINE_string(host, "localhost", "host to join");
DEFINE_int32(port, 2022, "port to connect on");
DEFINE_string(id, "", "Unique ID assigned to this session");
DEFINE_string(passkey, "", "Passkey to encrypt/decrypt packets");
DEFINE_string(idpasskeyfile, "",
              "File containing client ID and key to encrypt/decrypt packets");
DEFINE_string(command, "", "Command to run immediately after connecting");
DEFINE_string(portforward, "",
              "Array of source:destination ports or "
              "srcStart-srcEnd:dstStart-dstEnd (inclusive) port ranges (e.g. "
              "10080:80,10443:443, 10090-10092:8000-8002)");

shared_ptr<ClientConnection> createClient() {
  string id = FLAGS_id;
  string passkey = FLAGS_passkey;
  if (FLAGS_idpasskeyfile.length() > 0) {
    // Check for passkey file
    std::ifstream t(FLAGS_idpasskeyfile.c_str());
    std::stringstream buffer;
    buffer << t.rdbuf();
    string idpasskeypair = buffer.str();
    // Trim whitespace
    idpasskeypair.erase(idpasskeypair.find_last_not_of(" \n\r\t") + 1);
    size_t slashIndex = idpasskeypair.find("/");
    if (slashIndex == string::npos) {
      LOG(FATAL) << "Invalid idPasskey id/key pair: " << idpasskeypair;
    } else {
      id = idpasskeypair.substr(0, slashIndex);
      passkey = idpasskeypair.substr(slashIndex + 1);
      LOG(INFO) << "ID PASSKEY: " << id << " " << passkey << endl;
    }
    // Delete the file with the passkey
    remove(FLAGS_idpasskeyfile.c_str());
  }
  if (passkey.length() == 0 || id.length() == 0) {
    cout << "Unless you are doing development on Eternal Terminal,\nplease do "
            "not call etclient directly.\n\nThe et launcher (run on the "
            "client) calls etclient with the correct parameters.\nThis ensures "
            "a secure connection.\n\nIf you intended to call etclient "
            "directly, please provide a passkey\n(run \"etclient --help\" for "
            "details)."
         << endl;
    exit(1);
  }
  if (passkey.length() != 32) {
    LOG(FATAL) << "Invalid/missing passkey: " << passkey << " "
               << passkey.length();
  }

  InitialPayload payload;

  shared_ptr<SocketHandler> clientSocket(new UnixSocketHandler());
  shared_ptr<ClientConnection> client = shared_ptr<ClientConnection>(
      new ClientConnection(clientSocket, FLAGS_host, FLAGS_port, id, passkey));

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
  VLOG(1) << "Client created with id: " << client->getId() << endl;

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
              << " " << win->ws_xpixel << " " << win->ws_ypixel << endl;
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

int main(int argc, char** argv) {
  gflags::SetVersionString(string(ET_VERSION));
  ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  FLAGS_logbufsecs = 0;
  FLAGS_logbuflevel = google::GLOG_INFO;
  srand(1);

  Options options = {
    NULL, //username
    NULL, //host
    NULL, //sshdir
    NULL, //knownhosts
    NULL, //ProxyCommand
    0, //timeout
    0, //port
    0, //StrictHostKeyChecking
    0, //ssh2
    0, //ssh1
    NULL, //gss_server_identity
    NULL, //gss_client_identity
    0 //gss_delegate_creds
  };
  char* home_dir = ssh_get_user_home_dir();
  ssh_options_set(&options, SSH_OPTIONS_HOST, FLAGS_host.c_str());
  /* First parse user-specific ssh config, then system-wide config. */
  parse_ssh_config_file(&options, string(home_dir) + "/.ssh/config");
  parse_ssh_config_file(&options, "/etc/ssh/ssh_config");
  cout << "connecting to " << options.host << endl;
  FLAGS_host = string(options.host);

  globalClient = createClient();
  shared_ptr<UnixSocketHandler> socketHandler =
      static_pointer_cast<UnixSocketHandler>(globalClient->getSocketHandler());

  // Whether the TE should keep running.
  bool run = true;

// TE sends/receives data to/from the shell one char at a time.
#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

  time_t keepaliveTime = time(NULL) + 5;
  bool waitingOnKeepalive = false;

  if (FLAGS_command.length()) {
    LOG(INFO) << "Got command: " << FLAGS_command << endl;
    et::TerminalBuffer tb;
    tb.set_buffer(FLAGS_command + "; exit\n");

    char c = et::PacketType::TERMINAL_BUFFER;
    string headerString(1, c);
    globalClient->writeMessage(headerString);
    globalClient->writeProto(tb);
  }

  PortForwardClientRouter portForwardRouter;
  try {
    if (FLAGS_portforward.length()) {
      auto j = split(FLAGS_portforward, ',');
      for (auto& pair : j) {
        vector<string> sourceDestination = split(pair, ':');
        try {
          if (sourceDestination[0].find('-') != string::npos &&
              sourceDestination[1].find('-') != string::npos) {
            vector<string> sourcePortRange = split(sourceDestination[0], '-');
            int sourcePortStart = stoi(sourcePortRange[0]);
            int sourcePortEnd = stoi(sourcePortRange[1]);

            vector<string> destinationPortRange =
                split(sourceDestination[1], '-');
            int destinationPortStart = stoi(destinationPortRange[0]);
            int destinationPortEnd = stoi(destinationPortRange[1]);

            if (sourcePortEnd - sourcePortStart !=
                destinationPortEnd - destinationPortStart) {
              cout << "source/destination port range don't match" << endl;
              exit(1);
            } else {
              int portRangeLength = sourcePortEnd - sourcePortStart + 1;
              for (int i = 0; i < portRangeLength; ++i) {
                portForwardRouter.addListener(
                    shared_ptr<PortForwardClientListener>(
                        new PortForwardClientListener(
                            socketHandler, sourcePortStart + i,
                            destinationPortStart + i)));
              }
            }
          } else {
            int sourcePort = stoi(sourceDestination[0]);
            int destinationPort = stoi(sourceDestination[1]);

            portForwardRouter.addListener(shared_ptr<PortForwardClientListener>(
                new PortForwardClientListener(socketHandler, sourcePort,
                                              destinationPort)));
          }
        } catch (const std::logic_error& lr) {
          cout << "Logic error: " << lr.what() << endl;
          exit(1);
        }
      }
    }
  } catch (const std::runtime_error& ex) {
    cout << "Error establishing port forward: " << ex.what() << endl;
    exit(1);
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
          keepaliveTime = time(NULL) + 5;
        } else {
          LOG(FATAL) << "Got an error reading from stdin: " << rc;
        }
      }

      if (clientFd > 0 && FD_ISSET(clientFd, &rfd)) {
        while (globalClient->hasData()) {
          string packetTypeString;
          if (!globalClient->readMessage(&packetTypeString)) {
            break;
          }
          if (packetTypeString.length() != 1) {
            LOG(FATAL) << "Invalid packet header size: "
                       << packetTypeString.length();
          }
          char packetType = packetTypeString[0];
          switch (packetType) {
            case et::PacketType::TERMINAL_BUFFER: {
              // Read from the server and write to our fake terminal
              et::TerminalBuffer tb =
                  globalClient->readProto<et::TerminalBuffer>();
              const string& s = tb.buffer();
              // VLOG(1) << "Got byte: " << int(b) << " " << char(b) << " " <<
              // globalClient->getReader()->getSequenceNumber();
              keepaliveTime = time(NULL) + 1;
              FATAL_FAIL(writeAll(STDOUT_FILENO, &s[0], s.length()));
              break;
            }
            case et::PacketType::KEEP_ALIVE:
              waitingOnKeepalive = false;
              break;
            case PacketType::PORT_FORWARD_RESPONSE: {
              PortForwardResponse pfr =
                  globalClient->readProto<PortForwardResponse>();
              if (pfr.has_error()) {
                LOG(INFO) << "Could not connect to server through tunnel: "
                          << pfr.error();
                portForwardRouter.closeClientFd(pfr.clientfd());
              } else {
                LOG(INFO) << "Received socket/fd map from server: "
                          << pfr.socketid() << " " << pfr.clientfd();
                portForwardRouter.addSocketId(pfr.socketid(), pfr.clientfd());
              }
              break;
            }
            case PacketType::PORT_FORWARD_DATA: {
              PortForwardData pwd = globalClient->readProto<PortForwardData>();
              LOG(INFO) << "Got data for socket: " << pwd.socketid();
              if (pwd.has_closed()) {
                LOG(INFO) << "Port forward socket closed: " << pwd.socketid();
                portForwardRouter.closeSocketId(pwd.socketid());
              } else if (pwd.has_error()) {
                LOG(INFO) << "Port forward socket errored: " << pwd.socketid();
                portForwardRouter.closeSocketId(pwd.socketid());
              } else {
                portForwardRouter.sendDataOnSocket(pwd.socketid(),
                                                   pwd.buffer());
              }
              break;
            }
            default:
              LOG(FATAL) << "Unknown packet type: " << int(packetType) << endl;
          }
        }
      }

      if (clientFd > 0 && keepaliveTime < time(NULL)) {
        keepaliveTime = time(NULL) + 5;
        if (waitingOnKeepalive) {
          LOG(INFO) << "Missed a keepalive, killing connection.";
          globalClient->closeSocket();
          waitingOnKeepalive = false;
        } else {
          VLOG(1) << "Writing keepalive packet";
          string s(1, (char)et::PacketType::KEEP_ALIVE);
          globalClient->writeMessage(s);
          waitingOnKeepalive = true;
        }
      }

      handleWindowChanged(&win);

      vector<PortForwardRequest> requests;
      vector<PortForwardData> dataToSend;
      portForwardRouter.update(&requests, &dataToSend);
      for (auto& pfr : requests) {
        char c = et::PacketType::PORT_FORWARD_REQUEST;
        string headerString(1, c);
        globalClient->writeMessage(headerString);
        globalClient->writeProto(pfr);
      }
      for (auto& pwd : dataToSend) {
        char c = PacketType::PORT_FORWARD_DATA;
        string headerString(1, c);
        globalClient->writeMessage(headerString);
        globalClient->writeProto(pwd);
      }
    } catch (const runtime_error& re) {
      LOG(ERROR) << "Error: " << re.what() << endl;
      tcsetattr(0, TCSANOW, &terminal_backup);
      cout << "Connection closing because of error: " << re.what() << endl;
      run = false;
    }
  }

  globalClient.reset();
  LOG(INFO) << "Client derefernced" << endl;
  tcsetattr(0, TCSANOW, &terminal_backup);
  cout << "Session terminated" << endl;
  return 0;
}
