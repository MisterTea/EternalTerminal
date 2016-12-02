#include "ServerClientConnection.hpp"

ServerClientConnection::ServerClientConnection(
  const std::shared_ptr<SocketHandler>& _socketHandler,
  int _clientId,
  int _socketFd,
  const string& key
  ) :
  socketHandler(_socketHandler),
  socketFd(_socketFd),
  clientId(_clientId) {
  reader = shared_ptr<BackedReader>(
    new BackedReader(
      socketHandler,
      shared_ptr<CryptoHandler>(new CryptoHandler(key)),
      _socketFd));
  writer = shared_ptr<BackedWriter>(
    new BackedWriter(
      socketHandler,
      shared_ptr<CryptoHandler>(new CryptoHandler(key)),
      _socketFd));
}

bool ServerClientConnection::hasData() {
  return reader->hasData();
}

ssize_t ServerClientConnection::read(void* buf, size_t count) {
  ssize_t bytesRead = reader->read(buf, count);
  if (bytesRead == -1 && errno == ECONNRESET) {
    // The connection has reset, close the socket and invalidate, then
    // return 0 bytes
    closeSocket();
    bytesRead = 0;
  }
  return bytesRead;
}

ssize_t ServerClientConnection::readAll(void* buf, size_t count) {
  size_t pos=0;
  while (pos<count) {
    ssize_t bytesRead = read(((char*)buf) + pos, count - pos);
    if (bytesRead < 0) {
      VLOG(1) << "Failed a call to readAll: %s\n" << strerror(errno);
      throw std::runtime_error("Failed a call to readAll");
    }
    pos += bytesRead;
  }
  return count;
}

ssize_t ServerClientConnection::write(const void* buf, size_t count) {
  BackedWriterWriteState bwws = writer->write(buf, count);

  if(bwws == BackedWriterWriteState::SKIPPED) {
    return 0;
  }

  if(bwws == BackedWriterWriteState::WROTE_WITH_FAILURE) {
    // Error writing.
    if (errno == EPIPE) {
      // The connection has been severed, handle and hide from the caller
      closeSocket();
    } else {
      throw runtime_error("Oops");
    }
  }

  return count;
}

void ServerClientConnection::writeAll(const void* buf, size_t count) {
  while(true) {
    if(write(buf, count)) {
      return;
    }
    sleep(0);
  }
}

void ServerClientConnection::closeSocket() {
  if (socketFd == -1) {
    throw std::runtime_error("Tried to close a non-existent socket");
  }
  reader->invalidateSocket();
  writer->invalidateSocket();
  socketFd = -1;
  socketHandler->close(socketFd);
  VLOG(1) << "Closed socket\n";
}
