#ifndef __ETERNAL_TCP_SERVER_CONNECTION__
#define __ETERNAL_TCP_SERVER_CONNECTION__

#include "Headers.hpp"

#include "ServerClientConnection.hpp"
#include "SocketHandler.hpp"

namespace et {
class ServerConnectionHandler {
 public:
  virtual ~ServerConnectionHandler() {}

  virtual bool newClient(
      shared_ptr<ServerClientConnection> serverClientState) = 0;
};

class ServerConnection {
 public:
  explicit ServerConnection(std::shared_ptr<SocketHandler> socketHandler,
                            int port,
                            shared_ptr<ServerConnectionHandler> serverHandler);

  ~ServerConnection();

  inline bool clientKeyExists(const string& clientId) {
    return clientKeys.find(clientId) != clientKeys.end();
  }

  inline bool clientConnectionExists(const string& clientId) {
    return clientConnections.find(clientId) != clientConnections.end();
  }

  inline shared_ptr<SocketHandler> getSocketHandler() { return socketHandler; }

  void acceptNewConnection(int fd);

  void close();

  inline void addClientKey(const string& id, const string& passkey) {
    clientKeys[id] = passkey;
  }

  void clientHandler(int clientSocketFd);

  void newClientConnection(const string& clientId, int socketFd);

  bool removeClient(const string& id);

  shared_ptr<ServerClientConnection> getClientConnection(
      const string& clientId) {
    auto it = clientConnections.find(clientId);
    if (it == clientConnections.end()) {
      LOG(FATAL)
          << "Error: Tried to get a client connection that doesn't exist";
    }
    return it->second;
  }

 protected:
  shared_ptr<SocketHandler> socketHandler;
  int port;
  shared_ptr<ServerConnectionHandler> serverHandler;
  bool stop;
  std::unordered_map<string, string> clientKeys;
  std::unordered_map<string, shared_ptr<ServerClientConnection> >
      clientConnections;
};
}  // namespace et

#endif  // __ETERNAL_TCP_SERVER_CONNECTION__
