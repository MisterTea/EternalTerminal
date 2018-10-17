#ifndef __ET_CLIENT_CONNECTION__
#define __ET_CLIENT_CONNECTION__

#include "Headers.hpp"

#include "Connection.hpp"
#include "SocketEndpoint.hpp"

namespace et {
extern const int NULL_CLIENT_ID;

class ClientConnection : public Connection {
 public:
  ClientConnection(std::shared_ptr<SocketHandler> _socketHandler,
                   const SocketEndpoint& _endpoint, const string& _id,
                   const string& _key);

  virtual ~ClientConnection();

  bool connect();

  virtual void closeSocketAndMaybeReconnect();

  void waitReconnect();

 protected:
  void pollReconnect();

  SocketEndpoint remoteEndpoint;
  std::shared_ptr<std::thread> reconnectThread;
};
}  // namespace et

#endif  // __ET_SERVER_CONNECTION__
