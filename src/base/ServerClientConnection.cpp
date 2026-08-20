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

bool ServerClientConnection::recoverClient(int newSocketFd, bool forceReset) {
  // Detach the live session without closing it until recover succeeds, so a
  // failed/malicious reconnect cannot force-disconnect the victim.
  int oldSocketFd = -1;
  {
    lock_guard<std::recursive_mutex> guard(connectionMutex);
    oldSocketFd = socketFd;
    if (reader) {
      reader->invalidateSocket();
    }
    if (writer) {
      writer->invalidateSocket();
    }
    socketFd = -1;
  }

  bool success = recover(newSocketFd, forceReset);
  if (success) {
    // On the resume path the connection was constructed with this very fd
    // (oldSocketFd == newSocketFd); closing it would kill the live session.
    if (oldSocketFd != -1 && oldSocketFd != newSocketFd) {
      socketHandler->close(oldSocketFd);
    }
    return true;
  }

  if (oldSocketFd != -1) {
    lock_guard<std::recursive_mutex> guard(connectionMutex);
    socketFd = oldSocketFd;
    if (reader) {
      reader->revive(oldSocketFd, vector<string>());
    }
    if (writer) {
      writer->revive(oldSocketFd);
    }
  }
  return false;
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
