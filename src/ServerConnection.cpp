#include "ServerConnection.hpp"
namespace et {
ServerConnection::ServerConnection ( std::shared_ptr< SocketHandler > _socketHandler, int _port,
                                     shared_ptr< ServerConnectionHandler > _serverHandler, const string& _key )
    : socketHandler ( _socketHandler ),
      port ( _port ),
      serverHandler ( _serverHandler ),
      stop ( false ),
      key ( _key ) {}

ServerConnection::~ServerConnection ( ) {
  if ( clientConnectThread ) {
    clientConnectThread->join ( );
    clientConnectThread.reset ( );
  }
}

void ServerConnection::run ( ) {
  while ( !stop ) {
    VLOG ( 2 ) << "Listening for connection" << endl;
    int clientSocketFd = socketHandler->listen ( port );
    if ( clientSocketFd < 0 ) {
      sleep ( 1 );
      continue;
    }
    VLOG ( 1 ) << "SERVER: got client socket fd: " << clientSocketFd << endl;
    if ( clientConnectThread ) {
      clientConnectThread->join ( );
    }
    clientConnectThread =
        shared_ptr< thread > ( new thread ( &ServerConnection::clientHandler, this, clientSocketFd ) );
  }
}

void ServerConnection::close ( ) {
  stop = true;
  socketHandler->stopListening ( );
}

void ServerConnection::clientHandler ( int clientSocketFd ) {
  int clientId;
  try {
    socketHandler->readAll ( clientSocketFd, &clientId, sizeof ( int ) );
    if ( clientId == -1 ) {
      clientId = newClient ( clientSocketFd );
      shared_ptr< ServerClientConnection > serverClientState = getClient ( clientId );
      if ( serverHandler && !serverHandler->newClient ( serverClientState ) ) {
        // Destroy the new client
        removeClient ( clientId );
        socketHandler->close ( clientSocketFd );
      }
    } else {
      if ( !clientExists ( clientId ) ) {
        throw std::runtime_error ( "Tried to revive an unknown client" );
      }
      shared_ptr< ServerClientConnection > serverClientState = getClient ( clientId );
      serverClientState->recoverClient ( clientSocketFd );
    }
  } catch ( const runtime_error& err ) {
    // Comm failed, close the connection
    socketHandler->close ( clientSocketFd );
  }
}

int ServerConnection::newClient ( int socketFd ) {
  int clientId = rand ( );
  while ( clientExists ( clientId ) ) {
    clientId++;
    if ( clientId < 0 ) {
      throw std::runtime_error ( "Ran out of client ids" );
    }
  }
  VLOG ( 1 ) << "Created client with id " << clientId << endl;

  socketHandler->writeAll ( socketFd, &clientId, sizeof ( int ) );
  shared_ptr< ServerClientConnection > scc ( new ServerClientConnection ( socketHandler, clientId, socketFd, key ) );
  clients.insert ( std::make_pair ( clientId, scc ) );
  return clientId;
}
}
