#include "Headers.hpp"
#include "ClientConnection.hpp"
#include "ServerConnection.hpp"
#include "FlakyFakeSocketHandler.hpp"

#include <errno.h>
#include <sys/ioctl.h>
#include <termios.h>

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

#define FAIL_FATAL(X) if((X) == -1) { printf("Error: (%d), %s\n",errno,strerror(errno)); exit(errno); }

std::string commandToString(const char* cmd) {
    char buffer[128];
    std::string result = "";
    std::shared_ptr<FILE> pipe(popen(cmd, "r"), pclose);
    if (!pipe) throw std::runtime_error("popen() failed!");
    while (!feof(pipe.get())) {
        if (fgets(buffer, 128, pipe.get()) != NULL)
            result += buffer;
    }
    return result;
}

std::string getTerminal() {
#if __APPLE__
  return commandToString("dscl /Search -read \"/Users/$USER\" UserShell | awk '{print $2}'");
#else
  return commandToString("grep ^$(id -un): /etc/passwd | cut -d : -f 7-");
#endif
}

termios terminal_backup;

int main(int argc, char** argv) {
  srand(1);

  if (argc < 2) {
    cout << "usage: Terminal (username)" << endl;
  }
  string user = string(argv[1]);
  cout << "user: " << user << endl;
  passwd* pwd = getpwnam(user.c_str());
  if (pwd == NULL) {
    cout << "Could not find user " << user << endl;
    exit(1);
  }
  cout << "Got uid: " << pwd << endl;

  std::shared_ptr<FakeSocketHandler> serverSocket(new FakeSocketHandler());
  std::shared_ptr<FakeSocketHandler> clientSocket(new FlakyFakeSocketHandler(serverSocket, 1000));
  serverSocket->setRemoteHandler(clientSocket);

  std::array<char,64*1024> s;
  for (int a=0;a<64*1024 - 1;a++) {
    s[a] = rand()%26 + 'A';
  }
  s[64*1024 - 1] = 0;

  printf("Creating server\n");
  shared_ptr<ServerConnection> server = shared_ptr<ServerConnection>(
    new ServerConnection(serverSocket, 1000));
  globalServer = server;
  thread serverThread(runServer, server);

  shared_ptr<ClientConnection> client = shared_ptr<ClientConnection>(
    new ClientConnection(clientSocket, "localhost", 1000));
  globalClient = client;
  while(true) {
    try {
      client->connect();
    } catch (const runtime_error& err) {
      cout << "Connecting failed, retrying" << endl;
      continue;
    }
    break;
  }
  cout << "Client created with id: " << client->getClientId() << endl;
  int clientId = client->getClientId();
  shared_ptr<ClientState> serverClientState = globalServer->getClient(clientId);

  int masterfd;
  termios terminal_local;
  tcgetattr(0,&terminal_local);
  memcpy(&terminal_backup,&terminal_local,sizeof(struct termios));
  struct winsize win = { 0, 0, 0, 0 };
  ioctl(1, TIOCGWINSZ, &win);
  cfmakeraw(&terminal_local);
  tcsetattr(0,TCSANOW,&terminal_local);
  cout << win.ws_row << " "
       << win.ws_col << " "
       << win.ws_xpixel << " "
       << win.ws_ypixel << endl;


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
    setuid(pwd->pw_uid);
    setgid(pwd->pw_gid);
    terminal = terminal.substr(0,terminal.length()-1);
    cout << "Child process " << terminal << endl;
    execl("/bin/bash", "/bin/bash", NULL);
    //execl(terminal.c_str(), terminal.c_str(), NULL);
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
          serverClientState->writer->write(&b, 1);
        } else if (rc==0)
          run = false;
        else
          cout << "This shouldn't happen\n";
      }

      // Check for data to send.
      if (FD_ISSET(STDIN_FILENO, &rfd))
      {
        // Read from stdin and write to our client that will then send it to the server.
        read(STDIN_FILENO, &b, 1);
        globalClient->write(&b,1);
      }

      while (globalClient->hasData()) {
        globalClient->read(&b, 1);
        write(STDOUT_FILENO, &b, 1);
      }

      while (serverClientState->reader->hasData()) {
        // Read from the server and write to our fake terminal
        serverClientState->reader->read(&b,1);
        write(masterfd, &b, 1);
      }
    }
    break;
  }

  tcsetattr(0,TCSANOW,&terminal_backup);
  cout << "Shutting down server" << endl;
  server->close();
  serverThread.join();
  cout << "Server shut down" << endl;
  serverClientState.reset();
  cout << "ServerClientState down" << endl;
  globalServer.reset();
  server.reset();
  cout << "Server dereferenced" << endl;
  globalClient.reset();
  client.reset();
  cout << "Client derefernced" << endl;
  return 0;
}
