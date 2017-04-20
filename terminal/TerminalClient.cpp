#include "ClientConnection.hpp"
#include "CryptoHandler.hpp"
#include "FlakyFakeSocketHandler.hpp"
#include "Headers.hpp"
#include "ServerConnection.hpp"
#include "SocketUtils.hpp"
#include "UnixSocketHandler.hpp"

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

#define FAIL_FATAL(X)                                     \
  if ((X) == -1) {                                        \
    printf("Error: (%d), %s\n", errno, strerror(errno));  \
    exit(errno);                                          \
  }

termios terminal_backup;

DEFINE_string(host, "localhost", "host to join");
DEFINE_int32(port, 2022, "port to connect on");
DEFINE_string(id, "", "Unique ID assigned to this session");
DEFINE_string(passkey, "", "Passkey to encrypt/decrypt packets");
DEFINE_string(idpasskeyfile, "", "File containing client ID and key to encrypt/decrypt packets");

int main(int argc, char** argv) {
  ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  FLAGS_logbufsecs = 0;
  FLAGS_logbuflevel = google::GLOG_INFO;
  srand(1);

  std::shared_ptr<SocketHandler> clientSocket(new UnixSocketHandler());

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
      passkey = idpasskeypair.substr(slashIndex+1);
      LOG(INFO) << "ID PASSKEY: " << id << " " << passkey << endl;
    }
    // Delete the file with the passkey
    remove(FLAGS_idpasskeyfile.c_str());
  }
  if (passkey.length() == 0 || id.length()==0) {
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
  winsize win;
  ioctl(1, TIOCGWINSZ, &win);
  TerminalInfo* ti = payload.mutable_terminal();
  ti->set_row(win.ws_row);
  ti->set_column(win.ws_col);
  ti->set_width(win.ws_xpixel);
  ti->set_height(win.ws_ypixel);
  char* term = getenv("TERM");
  if (term) {
    LOG(INFO) << "Sending command to set terminal to " << term;
    // Set terminal
    string s = std::string("TERM=") + std::string(term);
    payload.add_environmentvar(s);
  }

  shared_ptr<ClientConnection> client = shared_ptr<ClientConnection>(
      new ClientConnection(clientSocket, FLAGS_host, FLAGS_port, id, passkey));
  globalClient = client;
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

  termios terminal_local;
  tcgetattr(0, &terminal_local);
  memcpy(&terminal_backup, &terminal_local, sizeof(struct termios));
  cfmakeraw(&terminal_local);
  tcsetattr(0, TCSANOW, &terminal_local);

  // Whether the TE should keep running.
  bool run = true;

  // TE sends/receives data to/from the shell one char at a time.
#define BUF_SIZE (1024)
  char b[BUF_SIZE];

  time_t keepaliveTime = time(NULL) + 5;
  bool waitingOnKeepalive = false;

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
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, NULL, NULL, &tv);

    try {
      // Check for data to send.
      if (FD_ISSET(STDIN_FILENO, &rfd)) {
        // Read from stdin and write to our client that will then send it to the
        // server.
        int rc = read(STDIN_FILENO, b, BUF_SIZE);
        FAIL_FATAL(rc);
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

      winsize tmpwin;
      ioctl(1, TIOCGWINSZ, &tmpwin);
      if (win.ws_row != tmpwin.ws_row || win.ws_col != tmpwin.ws_col ||
          win.ws_xpixel != tmpwin.ws_xpixel ||
          win.ws_ypixel != tmpwin.ws_ypixel) {
        win = tmpwin;
        LOG(INFO) << "Window size changed: " << win.ws_row << " " << win.ws_col
                  << " " << win.ws_xpixel << " " << win.ws_ypixel << endl;
        TerminalInfo ti;
        ti.set_row(win.ws_row);
        ti.set_column(win.ws_col);
        ti.set_width(win.ws_xpixel);
        ti.set_height(win.ws_ypixel);
        string s(1, (char)et::PacketType::TERMINAL_INFO);
        globalClient->writeMessage(s);
        globalClient->writeProto(ti);
      }

    } catch (const runtime_error& re) {
      LOG(ERROR) << "Error: " << re.what() << endl;
      tcsetattr(0, TCSANOW, &terminal_backup);
      cout << "Connection closing because of error: " << re.what() << endl;
      run = false;
    }
  }

  globalClient.reset();
  client.reset();
  LOG(INFO) << "Client derefernced" << endl;
  tcsetattr(0, TCSANOW, &terminal_backup);
  cout << "Session terminated" << endl;
  return 0;
}
