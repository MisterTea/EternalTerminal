#include "ServerConnection.hpp"

ServerConnection::ServerConnection(
  std::shared_ptr<SocketHandler> _socketHandler
  ) :
  socketHandler(_socketHandler) {
}

int ServerConnection::newClient(int socketFd) {
  int clientId = rand();
  while (clientExists(clientId)) {
    clientId++;
    if (clientId<0) {
      throw new std::runtime_error("Ran out of client ids");
    }
  }

  clients.insert(std::make_pair(clientId, ClientState(socketHandler,clientId,socketFd)));
  return clientId;
}

bool ServerConnection::recoverClient(int clientId, int socketFd) {
  // fix revive
  clients.find(clientId)->second.revive(socketFd, "");
  return true;
}
