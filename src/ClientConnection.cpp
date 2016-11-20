#include "ClientConnection.hpp"

const int NULL_CLIENT_ID = -1;

ClientConnection::ClientConnection(
  std::shared_ptr<SocketHandler> _socketHandler,
  const std::string& _hostname,
  int _port
  ) :
  socketHandler(_socketHandler),
  hostname(_hostname),
  port(_port) {
  socketFd = socketHandler->connect(hostname, port);
  socketHandler->writeAll(socketFd, &NULL_CLIENT_ID, sizeof(int));
  socketHandler->readAll(socketFd, &clientId, sizeof(int));
  reader = std::shared_ptr<BackedReader>(new BackedReader(socketHandler, socketFd));
  writer = std::shared_ptr<BackedWriter>(new BackedWriter(socketHandler, socketFd));
}

ssize_t ClientConnection::read(void* buf, size_t count) {
  ssize_t bytesRead = reader->read(buf, count);
  if (bytesRead == -1 && errno == ECONNRESET) {
    // The connection has reset, close the socket and invalidate, then
    // return 0 bytes
    closeSocket();
    bytesRead = 0;
  }
  return bytesRead;
}

ssize_t ClientConnection::write(const void* buf, size_t count) {
  ssize_t bytesWritten = writer->write(buf, count);
  if(bytesWritten == -1) {
    // Error writing.
    if (errno == EPIPE) {
      // The connection has been severed, handle and hide from the caller
      // TODO: Start backing up circular buffer to disk/vector
      closeSocket();

      // Try to write again, will go immediately to backup buffer
      return write(buf, count);
    }
  }

  // Success or some other error.
  return bytesWritten;
}

void ClientConnection::closeSocket() {
  if (socketFd == -1) {
    throw new std::runtime_error("Tried to close a non-existent socket");
  }
  socketHandler->close(socketFd);
  reader->invalidateSocket();
  writer->invalidateSocket();
  socketFd = -1;

  // Spin up a thread to poll for reconnects
  reconnectThread = std::shared_ptr<std::thread>(new std::thread(&ClientConnection::pollReconnect, this));
}

void ClientConnection::pollReconnect() {
  while (socketFd == -1) {
    fprintf(stderr, "Waiting to retry...\n");
    sleep(3000);
    int newSocketFd = socketHandler->connect(hostname, port);
    if (newSocketFd != -1) {
      int64_t localReaderSequenceNumber = reader->getSequenceNumber();
      socketHandler->writeAll(newSocketFd, &clientId, sizeof(int));
      socketHandler->writeAll(newSocketFd, &localReaderSequenceNumber, sizeof(int64_t));
      int64_t remoteReaderSequenceNumber;
      socketHandler->readAll(newSocketFd, &remoteReaderSequenceNumber, sizeof(int64_t));
      int64_t readerCatchupBytes;
      socketHandler->readAll(newSocketFd, &readerCatchupBytes, sizeof(int64_t));
      std::string readerCatchupString(readerCatchupBytes, (char)0);
      socketHandler->readAll(newSocketFd, &readerCatchupString[0], readerCatchupBytes);

      std::string writerCatchupString = writer->recover(remoteReaderSequenceNumber);
      socketHandler->writeAll(newSocketFd, &writerCatchupString[0], writerCatchupString.length());
      socketFd = newSocketFd;
      reader->revive(socketFd, readerCatchupString);
      writer->revive(socketFd);
    }
  }
}
