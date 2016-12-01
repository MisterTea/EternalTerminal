#ifndef __ETERNAL_TCP_SERVER_CONNECTION__
#define __ETERNAL_TCP_SERVER_CONNECTION__

#include "Headers.hpp"

#include "SocketHandler.hpp"
#include "BackedReader.hpp"
#include "BackedWriter.hpp"

class ClientState {
public:
  explicit ClientState(
    std::shared_ptr<SocketHandler> socketHandler,
    int _clientId,
    int _socketFd
    ) :
    clientId(_clientId),
    reader(new BackedReader(socketHandler, _socketFd)),
    writer(new BackedWriter(socketHandler, _socketFd)) {
  }

  void revive(int socketFd, const std::string &localBuffer) {
    reader->revive(socketFd, localBuffer);
    writer->revive(socketFd);
  }

  int clientId;
  std::shared_ptr<BackedReader> reader;
  std::shared_ptr<BackedWriter> writer;
};

class ServerConnection {
public:
  explicit ServerConnection(
    std::shared_ptr<SocketHandler> socketHandler,
    int port
    );

  ~ServerConnection();

  inline bool clientExists(int clientId) {
    return clients.find(clientId) != clients.end();
  }

  void run();
  inline void close() {
    stop=true;
  }

  void clientHandler(int clientSocketFd);

  int newClient(int socketFd);

  bool removeClient(int clientId) {
    return clients.erase(clientId) == 1;
  }

  bool recoverClient(int clientId, int socketFd);

  shared_ptr<ClientState> getClient(int clientId) {
    return clients.find(clientId)->second;
  }

  unordered_set<int> getClientIds() {
    unordered_set<int> retval;
    for (auto it : clients) {
      retval.insert(it.first);
    }
    return retval;
  }
protected:
  std::shared_ptr<SocketHandler> socketHandler;
  int port;
  bool stop;
  std::unordered_map<int, shared_ptr<ClientState> > clients;
  shared_ptr<thread> clientConnectThread;
};


#endif // __ETERNAL_TCP_SERVER_CONNECTION__
