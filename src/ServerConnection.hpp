#ifndef __ETERNAL_TCP_SERVER_CONNECTION__
#define __ETERNAL_TCP_SERVER_CONNECTION__

#include "Headers.hpp"

#include "ServerClientConnection.hpp"
#include "SocketHandler.hpp"

namespace et {
class ServerConnectionHandler {
 public:
  virtual ~ServerConnectionHandler ( ) {}

  virtual bool newClient ( shared_ptr< ServerClientConnection > serverClientState ) = 0;
};

class ServerConnection {
 public:
  explicit ServerConnection ( std::shared_ptr< SocketHandler > socketHandler, int port,
                              shared_ptr< ServerConnectionHandler > serverHandler, const string& key );

  ~ServerConnection ( );

  inline bool clientExists ( int clientId ) { return clients.find ( clientId ) != clients.end ( ); }

  void run ( );

  void close ( );

  void clientHandler ( int clientSocketFd );

  int newClient ( int socketFd );

  bool removeClient ( shared_ptr<ServerClientConnection> connection );

  shared_ptr< ServerClientConnection > getClient ( int clientId ) { return clients.find ( clientId )->second; }

  unordered_set< int > getClientIds ( ) {
    unordered_set< int > retval;
    for ( auto it : clients ) {
      retval.insert ( it.first );
    }
    return retval;
  }

 protected:
  shared_ptr< SocketHandler > socketHandler;
  int port;
  shared_ptr< ServerConnectionHandler > serverHandler;
  bool stop;
  std::unordered_map< int, shared_ptr< ServerClientConnection > > clients;
  shared_ptr< thread > clientConnectThread;
  string key;
};
}

#endif  // __ETERNAL_TCP_SERVER_CONNECTION__
