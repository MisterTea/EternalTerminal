#ifndef __ET_SERVER_CONNECTION__
#define __ET_SERVER_CONNECTION__

#include "Headers.hpp"

#include "ServerClientConnection.hpp"
#include "SocketEndpoint.hpp"
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
                            const SocketEndpoint& _serverEndpoint,
                            shared_ptr<ServerConnectionHandler> serverHandler);

  ~ServerConnection();

  inline bool clientKeyExists(const string& clientId) {
    lock_guard<std::recursive_mutex> guard(classMutex);
    return clientKeys.find(clientId) != clientKeys.end();
  }

  inline bool clientConnectionExists(const string& clientId) {
    return clientConnections.find(clientId) != clientConnections.end();
  }

  inline shared_ptr<SocketHandler> getSocketHandler() { return socketHandler; }

  bool acceptNewConnection(int fd);

  void shutdown();

  inline void addClientKey(const string& id, const string& passkey) {
    lock_guard<std::recursive_mutex> guard(classMutex);
    clientKeys[id] = passkey;
  }

  void clientHandler(int clientSocketFd);

  bool removeClient(const string& id);

  shared_ptr<ServerClientConnection> getClientConnection(
      const string& clientId) {
    lock_guard<std::recursive_mutex> guard(mutex);
    auto it = clientConnections.find(clientId);
    if (it == clientConnections.end()) {
      LOG(FATAL)
          << "Error: Tried to get a client connection that doesn't exist";
    }
    return it->second;
  }

 protected:
  shared_ptr<SocketHandler> socketHandler;
  SocketEndpoint serverEndpoint;
  shared_ptr<ServerConnectionHandler> serverHandler;
  bool stop;
  std::unordered_map<string, string> clientKeys;
  std::unordered_map<string, shared_ptr<ServerClientConnection>>
      clientConnections;
  ctpl::thread_pool clientHandlerThreadPool;
  recursive_mutex classMutex;
  mutex connectMutex;
};
}  // namespace et

#endif  // __ET_SERVER_CONNECTION__
