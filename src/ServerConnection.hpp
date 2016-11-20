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
    std::shared_ptr<SocketHandler> socketHandler
    );

  inline bool clientExists(int clientId) {
    return clients.find(clientId) != clients.end();
  }

  int newClient(int socketFd);

  bool removeClient(int clientId) {
    return clients.erase(clientId) == 1;
  }

  bool recoverClient(int clientId, int socketFd);
protected:
  std::shared_ptr<SocketHandler> socketHandler;
  std::unordered_map<int, ClientState> clients;
};


#endif // __ETERNAL_TCP_SERVER_CONNECTION__
