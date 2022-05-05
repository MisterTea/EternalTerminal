#include "ServerClientConnection.hpp"

namespace et {
ServerClientConnection::ServerClientConnection(
    const std::shared_ptr<SocketHandler>& _socketHandler,
    const string& clientId, int _socketFd, const string& key)
    : Connection(_socketHandler, clientId, key) {
  socketFd = _socketFd;
  reader = shared_ptr<BackedReader>(
      new BackedReader(socketHandler,
                       shared_ptr<CryptoHandler>(
                           new CryptoHandler(key, CLIENT_SERVER_NONCE_MSB)),
                       _socketFd));
  writer = shared_ptr<BackedWriter>(
      new BackedWriter(socketHandler,
                       shared_ptr<CryptoHandler>(
                           new CryptoHandler(key, SERVER_CLIENT_NONCE_MSB)),
                       _socketFd));
}

ServerClientConnection::~ServerClientConnection() {
  if (socketFd != -1) {
    closeSocket();
  }
}

bool ServerClientConnection::recoverClient(int newSocketFd) {
  {
    lock_guard<std::recursive_mutex> guard(connectionMutex);
    if (socketFd != -1) {
      closeSocket();
    }
  }
  return recover(newSocketFd);
}

bool ServerClientConnection::verifyPasskey(const string& targetKey) {
  // Do a string comparison without revealing timing information if an early
  // character mismatches, always loop through the entire string.
  const size_t commonSize =
      key.size() < targetKey.size() ? key.size() : targetKey.size();

  bool matchFailed = key.size() != targetKey.size();
  for (size_t i = 0; i < commonSize; ++i) {
    matchFailed |= key[i] != targetKey[i];
  }

  return !matchFailed;
}

}  // namespace et
