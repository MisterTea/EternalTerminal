#include "ServerClientConnection.hpp"

namespace et {
ServerClientConnection::ServerClientConnection(
    const std::shared_ptr<SocketHandler>& _socketHandler,
    int _clientId,  //
    int _socketFd,  //
    const string& key)
    : Connection(_socketHandler, key) {
  socketFd = _socketFd;
  clientId = _clientId;
  reader = shared_ptr<BackedReader>(new BackedReader(
      socketHandler, shared_ptr<CryptoHandler>(new CryptoHandler(key, CLIENT_SERVER_NONCE_MSB)),
      _socketFd));
  writer = shared_ptr<BackedWriter>(new BackedWriter(
      socketHandler, shared_ptr<CryptoHandler>(new CryptoHandler(key, SERVER_CLIENT_NONCE_MSB)),
      _socketFd));
}

ServerClientConnection::~ServerClientConnection() {
  if (socketFd != -1) {
    closeSocket();
  }
}

bool ServerClientConnection::recoverClient(int newSocketFd) {
  closeSocket();
  return recover(newSocketFd);
}
}
