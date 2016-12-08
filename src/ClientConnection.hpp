#ifndef __ETERNAL_TCP_CLIENT_CONNECTION__
#define __ETERNAL_TCP_CLIENT_CONNECTION__

#include "Headers.hpp"

#include "Connection.hpp"

namespace et {
extern const int NULL_CLIENT_ID;

class ClientConnection : public Connection {
 public:
  ClientConnection ( std::shared_ptr< SocketHandler > _socketHandler,  //
                     const std::string& hostname,                      //
                     int port,                                         //
                     const string& key );

  virtual ~ClientConnection ( );

  void connect ( );

 protected:
  virtual void closeSocket ( );
  void pollReconnect ( );

  std::string hostname;
  int port;
  std::shared_ptr< std::thread > reconnectThread;
};
}

#endif  // __ETERNAL_TCP_SERVER_CONNECTION__
