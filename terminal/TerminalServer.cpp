#include "ClientConnection.hpp"
#include "ConsoleUtils.hpp"
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

#if __APPLE__
#include <util.h>
#elif __FreeBSD__
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <libutil.h>
#else
#include <pty.h>
#endif

#include "ETerminal.pb.h"

using namespace et;
namespace google {}
namespace gflags {}
using namespace google;
using namespace gflags;

shared_ptr<ServerConnection> globalServer;

void halt();

#define FAIL_FATAL(X)                                              \
  if ((X) == -1) {                                                 \
    LOG(FATAL) << "Error: (" << errno << "): " << strerror(errno); \
  }

termios terminal_backup;

DEFINE_int32(port, 10022, "Port to listen on");
DEFINE_string(passkey, "", "Passkey to encrypt/decrypt packets");
DEFINE_string(passkeyfile, "", "Passkey file to encrypt/decrypt packets");

thread* terminalThread = NULL;
void runTerminal(shared_ptr<ServerClientConnection> serverClientState,
                 int masterfd) {
  string disconnectBuffer;

  // Whether the TE should keep running.
  bool run = true;

// TE sends/receives data to/from the shell one char at a time.
#define BUF_SIZE (1024)
  char b[BUF_SIZE];

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
    FD_SET(masterfd, &rfd);
    FD_SET(STDIN_FILENO, &rfd);
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    select(masterfd + 1, &rfd, &wfd, &efd, &tv);

    try {
      // Check for data to receive; the received
      // data includes also the data previously sent
      // on the same master descriptor (line 90).
      if (FD_ISSET(masterfd, &rfd)) {
        // Read from fake terminal and write to server
        memset(b, 0, BUF_SIZE);
        int rc = read(masterfd, b, BUF_SIZE);
        if (rc > 0) {
          // VLOG(2) << "Sending bytes: " << int(b) << " " << char(b) << " "
          // << serverClientState->getWriter()->getSequenceNumber();
          char c = et::PacketType::TERMINAL_BUFFER;
          serverClientState->writeMessage(string(1, c));
          string s(b, rc);
          et::TerminalBuffer tb;
          tb.set_buffer(s);
          serverClientState->writeProto(tb);
        } else {
          LOG(INFO) << "Terminal session ended";
          run = false;
          globalServer->removeClient(serverClientState);
          break;
        }
      }

      while (serverClientState->hasData()) {
        string packetTypeString;
        if (!serverClientState->readMessage(&packetTypeString)) {
          break;
        }
        char packetType = packetTypeString[0];
        switch (packetType) {
          case et::PacketType::TERMINAL_BUFFER: {
            // Read from the server and write to our fake terminal
            et::TerminalBuffer tb =
                serverClientState->readProto<et::TerminalBuffer>();
            const string& s = tb.buffer();
            // VLOG(2) << "Got byte: " << int(b) << " " << char(b) << " " <<
            // serverClientState->getReader()->getSequenceNumber();
            FATAL_FAIL(writeAll(masterfd, &s[0], s.length()));
            break;
          }
          case et::PacketType::KEEP_ALIVE: {
            // Echo keepalive back to client
            VLOG(1) << "Got keep alive";
            char c = et::PacketType::KEEP_ALIVE;
            serverClientState->writeMessage(string(1, c));
            break;
          }
          case et::PacketType::TERMINAL_INFO: {
            VLOG(1) << "Got terminal info";
            et::TerminalInfo ti =
                serverClientState->readProto<et::TerminalInfo>();
            winsize tmpwin;
            tmpwin.ws_row = ti.row();
            tmpwin.ws_col = ti.column();
            tmpwin.ws_xpixel = ti.width();
            tmpwin.ws_ypixel = ti.height();
            ioctl(masterfd, TIOCSWINSZ, &tmpwin);
            break;
          }
          default:
            LOG(FATAL) << "Unknown packet type: " << int(packetType) << endl;
        }
      }
    } catch (const runtime_error& re) {
      LOG(ERROR) << "Error: " << re.what();
      cerr << "Error: " << re.what();
      serverClientState->closeSocket();
      // If the client disconnects the session, it shuoldn't end
      // because the client may be starting a new one.  TODO: Start a
      // timer which eventually kills the server.

      // run=false;
    }
  }
  serverClientState.reset();
  halt();
}

void startTerminal(shared_ptr<ServerClientConnection> serverClientState,
                   InitialPayload payload) {
  const TerminalInfo& ti = payload.terminal();
  winsize win;
  win.ws_row = ti.row();
  win.ws_col = ti.column();
  win.ws_xpixel = ti.width();
  win.ws_ypixel = ti.height();
  for (const string& it : payload.environmentvar()) {
    size_t equalsPos = it.find("=");
    if (equalsPos == string::npos) {
      LOG(FATAL) << "Invalid environment variable";
    }
    string name = it.substr(0, equalsPos);
    string value = it.substr(equalsPos + 1);
    setenv(name.c_str(), value.c_str(), 1);
  }

  int masterfd;
  std::string terminal = getTerminal();

  pid_t pid = forkpty(&masterfd, NULL, NULL, &win);
  switch (pid) {
    case -1:
      FAIL_FATAL(pid);
    case 0:
      // child
      ProcessHelper::initChildProcess();

      VLOG(1) << "Closing server in fork" << endl;
      // Close server on client process
      globalServer->close();
      globalServer.reset();

      VLOG(1) << "Child process " << terminal << endl;
      execl(terminal.c_str(), terminal.c_str(), NULL);
      exit(0);
      break;
    default:
      // parent
      cout << "pty opened " << masterfd << endl;
      terminalThread = new thread(runTerminal, serverClientState, masterfd);
      break;
  }
}

class TerminalServerHandler : public ServerConnectionHandler {
  virtual bool newClient(shared_ptr<ServerClientConnection> serverClientState) {
    InitialPayload payload = serverClientState->readProto<InitialPayload>();
    startTerminal(serverClientState, payload);
    return true;
  }
};

int main(int argc, char** argv) {
  ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  FLAGS_logbufsecs = 0;
  FLAGS_logbuflevel = google::GLOG_INFO;
  srand(1);

  std::shared_ptr<UnixSocketHandler> serverSocket(new UnixSocketHandler());

  LOG(INFO) << "Creating server";

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
  if (passkey.length() == 0) {
    cout << "Unless you are doing development on Eternal Terminal,\nplease do "
            "not call etserver directly.\n\nThe et launcher (run on the "
            "client) uses ssh to remotely call etserver with the correct "
            "parameters.\nThis ensures a secure connection.\n\nIf you intended "
            "to call etserver directly, please provide a passkey\n(run "
            "\"etserver --help\" for details)."
         << endl;
    exit(1);
  }
  if (passkey.length() != 32) {
    LOG(FATAL) << "Invalid/missing passkey: " << passkey;
  }

  globalServer = shared_ptr<ServerConnection>(new ServerConnection(
      serverSocket, FLAGS_port,
      shared_ptr<TerminalServerHandler>(new TerminalServerHandler()), passkey));
  globalServer->run();
}

void halt() {
  cout << "Shutting down server" << endl;
  globalServer->close();
  cout << "Waiting for server to finish" << endl;
  sleep(3);
  exit(0);
}
