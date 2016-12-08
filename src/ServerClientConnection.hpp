#ifndef __ETERNAL_TCP_SERVER_CLIENT_CONNECTION__
#define __ETERNAL_TCP_SERVER_CLIENT_CONNECTION__

#include "Headers.hpp"

#include "Connection.hpp"

namespace et {
class ServerClientConnection : public Connection {
 public:
  explicit ServerClientConnection ( const std::shared_ptr< SocketHandler >& _socketHandler,  //
                                    int _clientId,                                           //
                                    int _socketFd,                                           //
                                    const string& key );

  ~ServerClientConnection ( ) { closeSocket ( ); }

  bool recoverClient ( int newSocketFd );

 protected:
};
}

#endif  // __ETERNAL_TCP_SERVER_CLIENT_CONNECTION__
