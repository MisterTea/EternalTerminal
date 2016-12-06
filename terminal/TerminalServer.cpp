#include "Headers.hpp"
#include "ClientConnection.hpp"
#include "ServerConnection.hpp"
#include "FlakyFakeSocketHandler.hpp"
#include "UnixSocketHandler.hpp"
#include "ProcessHelper.hpp"
#include "CryptoHandler.hpp"
#include "ConsoleUtils.hpp"

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

shared_ptr<ServerConnection> globalServer;
shared_ptr<ClientConnection> globalClient;

void runServer(
  std::shared_ptr<ServerConnection> server) {
  server->run();
}

void halt();

#define FAIL_FATAL(X) if((X) == -1) {                               \
    printf("Error: (%d), %s\n",errno,strerror(errno)); exit(errno); \
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
    char b;

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

      // Check for data to receive; the received
      // data includes also the data previously sent
      // on the same master descriptor (line 90).
      if (FD_ISSET(masterfd, &rfd))
      {
        // Read from fake terminal and write to server
        int rc = read(masterfd, &b, 1);
        FAIL_FATAL(rc);
        if (rc > 0) {
          //cout << "Writing byte to client...";
          serverClientState->write(&b, 1);
          //cout << "done" << endl;
        } else if (rc==0) {
          run = false;
          globalServer->removeClient(serverClientState->getId());
        } else
          cout << "This shouldn't happen\n";
      }

      while (serverClientState->hasData()) {
        // Read from the server and write to our fake terminal
        int rc = serverClientState->read(&b,1);
        FATAL_FAIL(rc);
        if(rc>0) {
          cout << "Getting byte from client..";
          write(masterfd, &b, 1);
          cout << "done: " << int(b) << endl;
        }
      } catch(const runtime_error& re) {
        cout << "Connection error: " << re.what() << endl;
        run=false;
      }
    }
    break;
  }

  serverClientState->close();
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
