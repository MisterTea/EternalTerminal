#include "Headers.hpp"
#include "ClientConnection.hpp"
#include "ServerConnection.hpp"
#include "FlakyFakeSocketHandler.hpp"
#include "UnixSocketHandler.hpp"
#include "ProcessHelper.hpp"
#include "CryptoHandler.hpp"
#include "ConsoleUtils.hpp"
#include "SocketUtils.hpp"

#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <pwd.h>

#if __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

#include "ETerminal.pb.h"

using namespace et;
shared_ptr<ServerConnection> globalServer;

void runServer(
  std::shared_ptr<ServerConnection> server) {
  server->run();
}

void halt();

#define FAIL_FATAL(X) if((X) == -1) {                               \
    LOG(FATAL) << "Error: (" << errno << "): " << strerror(errno); \
  }

termios terminal_backup;

DEFINE_int32(port, 10022, "Port to listen on");
DEFINE_string(passkey, "", "Passkey to encrypt/decrypt packets");

thread* terminalThread = NULL;
void runTerminal(shared_ptr<ServerClientConnection> serverClientState) {
  // TODO: Get window size from client
  struct winsize win = { 0, 0, 0, 0 };
  int masterfd;

  std::string terminal = getTerminal();

  pid_t pid = forkpty(
    &masterfd,
    NULL,
    NULL,
    &win);
  switch (pid) {
  case -1:
    FAIL_FATAL(pid);
  case 0:
    // child
    ProcessHelper::initChildProcess();

    // Not needed since server is running in userspace
    //setuid(pwd->pw_uid);
    //setgid(pwd->pw_gid);

    cout << "Child process " << terminal << endl;
    //execl("/bin/bash", "/bin/bash", NULL);
    execl(terminal.c_str(), terminal.c_str(), NULL);
    exit(0);
    break;
  default:
    // parent
    cout << "pty opened " << masterfd << endl;
    // Whether the TE should keep running.
    bool run = true;

    // TE sends/receives data to/from the shell one char at a time.
#define BUF_SIZE (1024)
    char b[BUF_SIZE];

    while (run)
    {
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
        if (FD_ISSET(masterfd, &rfd))
        {
          // Read from fake terminal and write to server
          memset(b,0,BUF_SIZE);
          int rc = read(masterfd, b, BUF_SIZE);
          FAIL_FATAL(rc);
          if (rc > 0) {
            //VLOG(2) << "Sending bytes: " << int(b) << " " << char(b) << " " << serverClientState->getWriter()->getSequenceNumber();
            char c = et::PacketType::TERMINAL_BUFFER;
            serverClientState->writeAll(&c,1);
            string s(b,rc);
            et::TerminalBuffer tb;
            tb.set_buffer(s);
            serverClientState->writeProto(tb);
          } else if (rc==0) {
            LOG(INFO) << "Terminal session ended";
            run = false;
            globalServer->removeClient(serverClientState);
            break;
          } else {
            LOG(FATAL) << "This shouldn't happen\n";
          }
        }

        while (serverClientState->hasData()) {
          char packetType;
          serverClientState->readAll(&packetType,1);
          switch (packetType) {
          case et::PacketType::TERMINAL_BUFFER:
          {
            // Read from the server and write to our fake terminal
            et::TerminalBuffer tb =
              serverClientState->readProto<et::TerminalBuffer>();
            const string& s = tb.buffer();
            //VLOG(2) << "Got byte: " << int(b) << " " << char(b) << " " << serverClientState->getReader()->getSequenceNumber();
            FATAL_FAIL(writeAll(masterfd, &s[0], s.length()));
            break;
          }
          case et::PacketType::KEEP_ALIVE:
          {
            // Echo keepalive back to client
            VLOG(1) << "Got keep alive";
            char c = et::PacketType::KEEP_ALIVE;
            serverClientState->writeAll(&c,1);
            break;
          }
          default:
            LOG(FATAL) << "Unknown packet type: " << int(packetType) << endl;
          }
        }
      } catch(const runtime_error& re) {
        cout << "Connection error: " << re.what() << endl;
        run=false;
      }
    }
    break;
  }

  serverClientState.reset();
  halt();
}

class TerminalServerHandler : public ServerConnectionHandler {
  virtual bool newClient(
    shared_ptr<ServerClientConnection> serverClientState) {
    terminalThread = new thread(runTerminal, serverClientState);
    return true;
  }
};

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  FLAGS_logbufsecs = 0;
  FLAGS_logbuflevel = google::GLOG_INFO;
  srand(1);

  std::shared_ptr<UnixSocketHandler> serverSocket(new UnixSocketHandler());

  printf("Creating server\n");
  shared_ptr<ServerConnection> server = shared_ptr<ServerConnection>(
    new ServerConnection(
      serverSocket,
      FLAGS_port,
      shared_ptr<TerminalServerHandler>(new TerminalServerHandler()),
      FLAGS_passkey));
  globalServer = server;
  runServer(server);
}

void halt() {
  cout << "Shutting down server" << endl;
  globalServer->close();
  cout << "Waiting for server to finish" << endl;
  sleep(3);
  exit(0);
}
