#include "ClientConnection.hpp"
#include "CryptoHandler.hpp"
#include "FlakyFakeSocketHandler.hpp"
#include "Headers.hpp"
#include "ProcessHelper.hpp"
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
shared_ptr<ClientConnection> globalClient;

#define FAIL_FATAL(X)                                    \
  if ((X) == -1) {                                       \
    printf("Error: (%d), %s\n", errno, strerror(errno)); \
    exit(errno);                                         \
  }

termios terminal_backup;

DEFINE_string(host, "localhost", "host to join");
DEFINE_int32(port, 10022, "port to connect on");
DEFINE_string(passkey, "", "Passkey to encrypt/decrypt packets");
DEFINE_string(passkeyfile, "", "Passkey file to encrypt/decrypt packets");

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  FLAGS_logbufsecs = 0;
  FLAGS_logbuflevel = google::GLOG_INFO;
  srand(1);

  std::shared_ptr<SocketHandler> clientSocket(new UnixSocketHandler());

  string passkey = FLAGS_passkey;
  if (passkey.length() == 0 && FLAGS_passkeyfile.length() > 0) {
    // Check for passkey file
    std::ifstream t(FLAGS_passkeyfile.c_str());
    std::stringstream buffer;
    buffer << t.rdbuf();
    passkey = buffer.str();
    // Trim whitespace
    passkey.erase(passkey.find_last_not_of(" \n\r\t") + 1);
    // Delete the file with the passkey
    remove(FLAGS_passkeyfile.c_str());
  }
  if (passkey.length() != 32) {
    LOG(FATAL) << "Invalid/missing passkey: " << passkey << " "
               << passkey.length();
  }

  shared_ptr<ClientConnection> client = shared_ptr<ClientConnection>(
      new ClientConnection(clientSocket, FLAGS_host, FLAGS_port, passkey));
  globalClient = client;
  while (true) {
    try {
      client->connect();
    } catch (const runtime_error& err) {
      LOG(ERROR) << "Connecting to server failed: " << err.what() << endl;
      sleep(1);
      continue;
    }
    break;
  }
  cout << "Client created with id: " << client->getClientId() << endl;

  termios terminal_local;
  tcgetattr(0, &terminal_local);
  memcpy(&terminal_backup, &terminal_local, sizeof(struct termios));
  struct winsize win = {0, 0, 0, 0};
  cfmakeraw(&terminal_local);
  tcsetattr(0, TCSANOW, &terminal_local);

  // Whether the TE should keep running.
  bool run = true;

// TE sends/receives data to/from the shell one char at a time.
#define BUF_SIZE (1024)
  char b[BUF_SIZE];

  time_t keepaliveTime = time(NULL) + 5;
  bool waitingOnKeepalive = false;

  while (run) {
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    fd_set wfd;
    fd_set efd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_ZERO(&wfd);
    FD_ZERO(&efd);
    FD_SET(STDIN_FILENO, &rfd);
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    select(STDIN_FILENO + 1, &rfd, &wfd, &efd, &tv);

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
          globalClient->writeAll(&c, 1);
          globalClient->writeProto(tb);
          keepaliveTime = time(NULL) + 5;
        } else {
          LOG(FATAL) << "Got an error reading from stdin: " << rc;
        }
      }

      while (globalClient->hasData()) {
        char packetType;
        globalClient->readAll(&packetType, 1);
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

      if (keepaliveTime < time(NULL)) {
        keepaliveTime = time(NULL) + 5;
        if (waitingOnKeepalive) {
          LOG(INFO) << "Missed a keepalive, killing connection.";
          globalClient->closeSocket();
          waitingOnKeepalive = false;
        } else {
          VLOG(1) << "Writing keepalive packet";
          char c = et::PacketType::KEEP_ALIVE;
          globalClient->writeAll(&c, 1);
          waitingOnKeepalive = true;
        }
      }

      winsize tmpwin;
      ioctl(1, TIOCGWINSZ, &tmpwin);
      if (win.ws_row != tmpwin.ws_row || win.ws_col != tmpwin.ws_col ||
          win.ws_xpixel != tmpwin.ws_xpixel ||
          win.ws_ypixel != tmpwin.ws_ypixel) {
        win = tmpwin;
        cout << "Window size changed: " << win.ws_row << " " << win.ws_col
             << " " << win.ws_xpixel << " " << win.ws_ypixel << endl;
        TerminalInfo ti;
        ti.set_rows(win.ws_row);
        ti.set_columns(win.ws_col);
        ti.set_width(win.ws_xpixel);
        ti.set_height(win.ws_ypixel);
        char c = et::PacketType::TERMINAL_INFO;
        globalClient->writeAll(&c, 1);
        globalClient->writeProto(ti);
      }

    } catch (const runtime_error& re) {
      LOG(ERROR) << "Error: " << re.what() << endl;
      run = false;
    }

    usleep(1000);
  }

  tcsetattr(0, TCSANOW, &terminal_backup);
  globalClient.reset();
  client.reset();
  LOG(INFO) << "Client derefernced" << endl;
  return 0;
}
