#include "Headers.hpp"
#include "ClientConnection.hpp"
#include "ServerConnection.hpp"
#include "FlakyFakeSocketHandler.hpp"
#include "UnixSocketHandler.hpp"
#include "ProcessHelper.hpp"
#include "CryptoHandler.hpp"

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

#define FAIL_FATAL(X) if((X) == -1) { \
    printf("Error: (%d), %s\n",errno,strerror(errno)); exit(errno); \
  }

std::string commandToString(string cmd) {
  char buffer[128];
  std::string result = "";
  std::shared_ptr<FILE> pipe(popen(cmd.c_str(), "r"), pclose);
  if (!pipe) throw std::runtime_error("popen() failed!");
  while (!feof(pipe.get())) {
    if (fgets(buffer, 128, pipe.get()) != NULL)
      result += buffer;
  }
  return result;
}

std::string getTerminal(string username) {
#if __APPLE__
  return commandToString(string("dscl /Search -read \"/Users/") + username + string("\" UserShell | awk '{print $2}'"));
#else
  return commandToString(string("grep ^") + username + string(": /etc/passwd | cut -d : -f 7-"));
#endif
}

termios terminal_backup;

DEFINE_int32(port, 10023, "Port to listen on");
DEFINE_string(passkey, "", "Passkey to encrypt/decrypt packets");

class TerminalServerHandler : public ServerConnectionHandler {
  virtual bool newClient(
    shared_ptr<ServerClientConnection> serverClientState) {
    passwd* pwd = getpwuid(getuid());

    // TODO: Get window size from client
    struct winsize win = { 0, 0, 0, 0 };
    int masterfd;

    std::string terminal = getTerminal(pwd->pw_name);

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

      terminal = terminal.substr(0,terminal.length()-1);
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
        tv.tv_usec = 100000;
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
            serverClientState->write(&b, 1);
          } else if (rc==0)
            run = false;
          else
            cout << "This shouldn't happen\n";
        }

        while (serverClientState->hasData()) {
          // Read from the server and write to our fake terminal
          int rc = serverClientState->read(&b,1);
          FATAL_FAIL(rc);
          if(rc>0) {
            write(masterfd, &b, 1);
          }
        }
      }
      break;
    }

    return 0;
  }
};

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
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
