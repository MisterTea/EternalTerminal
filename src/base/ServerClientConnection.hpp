#ifndef __ET_SERVER_CLIENT_CONNECTION__
#define __ET_SERVER_CLIENT_CONNECTION__

#include "Headers.hpp"

#include "Connection.hpp"

namespace et {
class ServerClientConnection : public Connection {
 public:
  explicit ServerClientConnection(
      const std::shared_ptr<SocketHandler>& _socketHandler,
      const string& clientId, int _socketFd, const string& key);

  virtual ~ServerClientConnection();

  bool recoverClient(int newSocketFd);

 protected:
};
}  // namespace et

#endif  // __ET_SERVER_CLIENT_CONNECTION__
