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
}

ClientConnection::~ClientConnection() {
  if (reconnectThread) {
    reconnectThread->join();
    reconnectThread.reset();
  }
}

void ClientConnection::connect() {
  try {
    cout << "Connecting" << endl;
    socketFd = socketHandler->connect(hostname, port);
    cout << "Sending null id" << endl;
    socketHandler->writeAllTimeout(socketFd, &NULL_CLIENT_ID, sizeof(int));
    cout << "Receiving client id" << endl;
    socketHandler->readAllTimeout(socketFd, &clientId, sizeof(int));
    cout << "Creating backed reader" << endl;
    reader = std::shared_ptr<BackedReader>(new BackedReader(socketHandler, socketFd));
    cout << "Creating backed writer" << endl;
    writer = std::shared_ptr<BackedWriter>(new BackedWriter(socketHandler, socketFd));
    cout << "Client Connection established" << endl;
  } catch (const runtime_error& err) {
    socketHandler->close(socketFd);
    throw err;
  }
}

bool ClientConnection::hasData() {
  return reader->hasData();
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

ssize_t ClientConnection::readAll(void* buf, size_t count) {
  size_t pos=0;
  while (pos<count) {
    ssize_t bytesRead = read(((char*)buf) + pos, count - pos);
    if (bytesRead < 0) {
      fprintf(stderr, "Failed a call to readAll: %s\n", strerror(errno));
      throw std::runtime_error("Failed a call to readAll");
    }
    pos += bytesRead;
  }
  return count;
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

ssize_t ClientConnection::writeAll(const void* buf, size_t count) {
  size_t pos=0;
  while (pos<count) {
    ssize_t bytesWritten = write(((const char*)buf) + pos, count - pos);
    if (bytesWritten < 0) {
      fprintf(stderr, "Failed a call to writeAll: %s\n", strerror(errno));
      throw std::runtime_error("Failed a call to writeAll");
    }
    pos += bytesWritten;
  }
  return count;
}

void ClientConnection::closeSocket() {
  if (socketFd == -1) {
    throw new std::runtime_error("Tried to close a non-existent socket");
  }
  reader->invalidateSocket();
  writer->invalidateSocket();
  socketFd = -1;
  socketHandler->close(socketFd);
  cout << "CLIENT: Closed socket\n";

  // Spin up a thread to poll for reconnects
  if (reconnectThread.get()) {
    reconnectThread->join();
  }
  if (socketFd == -1) {
    reconnectThread = std::shared_ptr<std::thread>(new std::thread(&ClientConnection::pollReconnect, this));
  }
}

void ClientConnection::pollReconnect() {
  while (socketFd == -1) {
    int newSocketFd = socketHandler->connect(hostname, port);
    if (newSocketFd != -1) {
      try {
        socketHandler->writeAllTimeout(newSocketFd, &clientId, sizeof(int));

        int64_t localReaderSequenceNumber = reader->getSequenceNumber();
        socketHandler->writeAllTimeout(newSocketFd, &localReaderSequenceNumber, sizeof(int64_t));
        int64_t remoteReaderSequenceNumber;
        socketHandler->readAllTimeout(newSocketFd, &remoteReaderSequenceNumber, sizeof(int64_t));

        std::string writerCatchupString = writer->recover(remoteReaderSequenceNumber);
        int64_t writerCatchupStringLength = writerCatchupString.length();
        socketHandler->writeAllTimeout(newSocketFd, &writerCatchupStringLength, sizeof(int64_t));
        socketHandler->writeAllTimeout(newSocketFd, &writerCatchupString[0], writerCatchupString.length());

        int64_t readerCatchupBytes;
        socketHandler->readAllTimeout(newSocketFd, &readerCatchupBytes, sizeof(int64_t));
        std::string readerCatchupString(readerCatchupBytes, (char)0);
        socketHandler->readAllTimeout(newSocketFd, &readerCatchupString[0], readerCatchupBytes);

        socketFd = newSocketFd;
        reader->revive(socketFd, readerCatchupString);
        writer->revive(socketFd);
        writer->unlock();
        break;
      } catch (const runtime_error& err) {
        cout << "Failed while recovering" << endl;
        writer->unlock();
      }
    } else {
      fprintf(stderr, "Waiting to retry...\n");
      sleep(1);
    }
  }
}
