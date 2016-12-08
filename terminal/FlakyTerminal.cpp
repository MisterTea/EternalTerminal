#include "ClientConnection.hpp"
#include "ConsoleUtils.hpp"
#include "FlakyFakeSocketHandler.hpp"
#include "Headers.hpp"
#include "ProcessHelper.hpp"
#include "ServerConnection.hpp"

#include <errno.h>
#include <sys/ioctl.h>
#include <termios.h>

#if __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

using namespace et;

shared_ptr< ServerConnection > globalServer;
shared_ptr< ClientConnection > globalClient;

void runServer ( std::shared_ptr< ServerConnection > server ) { server->run ( ); }

#define FAIL_FATAL( X )                                        \
  if ( ( X ) == -1 ) {                                         \
    printf ( "Error: (%d), %s\n", errno, strerror ( errno ) ); \
    exit ( errno );                                            \
  }

termios terminal_backup;

int main ( int argc, char** argv ) {
  google::InitGoogleLogging ( argv[ 0 ] );
  gflags::ParseCommandLineFlags ( &argc, &argv, true );

  srand ( 1 );

  std::shared_ptr< FakeSocketHandler > serverSocket ( new FakeSocketHandler ( ) );
  std::shared_ptr< FakeSocketHandler > clientSocket ( new FlakyFakeSocketHandler ( serverSocket, 1000 ) );
  serverSocket->setRemoteHandler ( clientSocket );

  std::array< char, 64 * 1024 > s;
  for ( int a = 0; a < 64 * 1024 - 1; a++ ) {
    s[ a ] = rand ( ) % 26 + 'A';
  }
  s[ 64 * 1024 - 1 ] = 0;

  printf ( "Creating server\n" );
  shared_ptr< ServerConnection > server = shared_ptr< ServerConnection > (
      new ServerConnection ( serverSocket, 1000, NULL, "12345678901234567890123456789012" ) );
  globalServer = server;
  thread serverThread ( runServer, server );

  shared_ptr< ClientConnection > client = shared_ptr< ClientConnection > (
      new ClientConnection ( clientSocket, "localhost", 1000, "12345678901234567890123456789012" ) );
  globalClient = client;
  while ( true ) {
    try {
      client->connect ( );
    } catch ( const runtime_error& err ) {
      cout << "Connecting failed, retrying" << endl;
      continue;
    }
    break;
  }
  cout << "Client created with id: " << client->getClientId ( ) << endl;
  int clientId = client->getClientId ( );
  shared_ptr< ServerClientConnection > serverClientState = globalServer->getClient ( clientId );

  int masterfd;
  termios terminal_local;
  tcgetattr ( 0, &terminal_local );
  memcpy ( &terminal_backup, &terminal_local, sizeof ( struct termios ) );
  struct winsize win = {0, 0, 0, 0};
  ioctl ( 1, TIOCGWINSZ, &win );
  cfmakeraw ( &terminal_local );
  tcsetattr ( 0, TCSANOW, &terminal_local );
  cout << win.ws_row << " " << win.ws_col << " " << win.ws_xpixel << " " << win.ws_ypixel << endl;

  std::string terminal = getTerminal ( );

  pid_t pid = forkpty ( &masterfd, NULL, NULL, &win );
  switch ( pid ) {
    case -1:
      FAIL_FATAL ( pid );
    case 0:
      // child
      ProcessHelper::initChildProcess ( );
      cout << "Child process " << terminal << endl;
      cout << "Child process " << terminal << endl;
      // execl("/bin/bash", "/bin/bash", NULL);
      execl ( terminal.c_str ( ), terminal.c_str ( ), NULL );
      exit ( 0 );
      break;
    default:
      // parent
      cout << "pty opened " << masterfd << endl;
      // Whether the TE should keep running.
      bool run = true;

      // TE sends/receives data to/from the shell one char at a time.
      char b;

      while ( run ) {
        // Data structures needed for select() and
        // non-blocking I/O.
        fd_set rfd;
        fd_set wfd;
        fd_set efd;
        timeval tv;

        FD_ZERO ( &rfd );
        FD_ZERO ( &wfd );
        FD_ZERO ( &efd );
        FD_SET ( masterfd, &rfd );
        FD_SET ( STDIN_FILENO, &rfd );
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        select ( masterfd + 1, &rfd, &wfd, &efd, &tv );

        // Check for data to receive; the received
        // data includes also the data previously sent
        // on the same master descriptor (line 90).
        if ( FD_ISSET ( masterfd, &rfd ) ) {
          // Read from fake terminal and write to server
          int rc = read ( masterfd, &b, 1 );
          FAIL_FATAL ( rc );
          if ( rc > 0 ) {
            serverClientState->write ( &b, 1 );
          } else if ( rc == 0 ) {
            LOG ( INFO ) << "Got empty read, connection is closed";
            run = false;
          } else {
            cout << "This shouldn't happen\n";
          }
        }

        // Check for data to send.
        if ( FD_ISSET ( STDIN_FILENO, &rfd ) ) {
          // Read from stdin and write to our client that will then send it to the server.
          read ( STDIN_FILENO, &b, 1 );
          globalClient->writeAll ( &b, 1 );
        }

        while ( globalClient->hasData ( ) ) {
          int rc = globalClient->read ( &b, 1 );
          FATAL_FAIL ( rc );
          if ( rc > 0 ) {
            write ( STDOUT_FILENO, &b, 1 );
          }
        }

        while ( serverClientState->hasData ( ) ) {
          // Read from the server and write to our fake terminal
          int rc = serverClientState->read ( &b, 1 );
          FATAL_FAIL ( rc );
          if ( rc > 0 ) {
            write ( masterfd, &b, 1 );
          }
        }
      }
      break;
  }

  tcsetattr ( 0, TCSANOW, &terminal_backup );
  cout << "Shutting down server" << endl;
  server->close ( );
  serverThread.join ( );
  cout << "Server shut down" << endl;
  serverClientState.reset ( );
  cout << "ServerClientState down" << endl;
  globalServer.reset ( );
  server.reset ( );
  cout << "Server dereferenced" << endl;
  globalClient.reset ( );
  client.reset ( );
  cout << "Client derefernced" << endl;
  return 0;
}
