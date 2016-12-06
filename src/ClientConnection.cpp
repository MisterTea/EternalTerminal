#include "ClientConnection.hpp"

const int NULL_CLIENT_ID = -1;

ClientConnection::ClientConnection(
  std::shared_ptr<SocketHandler> _socketHandler,
  const std::string& _hostname,
  int _port,
  const string& _key
  ) :
  socketHandler(_socketHandler),
  hostname(_hostname),
  port(_port),
  key(_key) {
}

ClientConnection::~ClientConnection() {
  if (reconnectThread) {
    reconnectThread->join();
    reconnectThread.reset();
  }
}

void ClientConnection::connect() {
  try {
    VLOG(1) << "Connecting" << endl;
    socketFd = socketHandler->connect(hostname, port);
    VLOG(1) << "Sending null id" << endl;
    socketHandler->writeAllTimeout(socketFd, &NULL_CLIENT_ID, sizeof(int));
    VLOG(1) << "Receiving client id" << endl;
    socketHandler->readAllTimeout(socketFd, &clientId, sizeof(int));
    VLOG(1) << "Creating backed reader" << endl;
    reader = std::shared_ptr<BackedReader>(
      new BackedReader(
        socketHandler,
        shared_ptr<CryptoHandler>(new CryptoHandler(key)),
        socketFd));
    VLOG(1) << "Creating backed writer" << endl;
    writer = std::shared_ptr<BackedWriter>(
      new BackedWriter(
        socketHandler,
        shared_ptr<CryptoHandler>(new CryptoHandler(key)),
        socketFd));
    VLOG(1) << "Client Connection established" << endl;
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
  if (bytesRead == -1) {
    if (errno == ECONNRESET || errno == ETIMEDOUT) {
      // The connection has reset, close the socket and invalidate, then
      // return 0 bytes
      closeSocket();
      bytesRead = 0;
    }
  }
  return bytesRead;
}

ssize_t ClientConnection::readAll(void* buf, size_t count) {
  size_t pos=0;
  while (pos<count) {
    ssize_t bytesRead = read(((char*)buf) + pos, count - pos);
    if (bytesRead < 0) {
      VLOG(1) << "Failed a call to readAll: %s\n" << strerror(errno);
      throw std::runtime_error("Failed a call to readAll");
    }
    pos += bytesRead;
    if(pos<count) {
      // Yield the processor
      sleep(0);
    }
  }
  return count;
}

ssize_t ClientConnection::write(const void* buf, size_t count) {
  BackedWriterWriteState bwws = writer->write(buf, count);

  if(bwws == BackedWriterWriteState::SKIPPED) {
    return 0;
  }

  if(bwws == BackedWriterWriteState::WROTE_WITH_FAILURE) {
    // Error writing.
    if (errno == EPIPE || errno == ETIMEDOUT) {
      // The connection has been severed, handle and hide from the caller
      closeSocket();

      // Tell the caller that bytes were written
    } else {
      throw runtime_error("Unhandled exception");
    }
  }

  // Success or some other error.
  return count;
}

void ClientConnection::writeAll(const void* buf, size_t count) {
  while(true) {
    if(write(buf, count)) {
      return;
    }
    sleep(0);
  }
}

void ClientConnection::closeSocket() {
  if (socketFd == -1) {
    LOG(ERROR) << "Tried to close a non-existent socket";
    return;
  }
  reader->invalidateSocket();
  writer->invalidateSocket();
  socketHandler->close(socketFd);
  socketFd = -1;
  VLOG(1) << "CLIENT: Closed socket\n";

  if (reconnectThread.get()) {
    reconnectThread->join();
    reconnectThread.reset();
  }
  if (socketFd == -1) {
    // Spin up a thread to poll for reconnects
    reconnectThread = std::shared_ptr<std::thread>(new std::thread(&ClientConnection::pollReconnect, this));
  }
}

void ClientConnection::pollReconnect() {
  while (socketFd == -1) {
    LOG(INFO) << "Trying to reconnect to " << hostname << ":" << port << endl;
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

        reader->revive(newSocketFd, readerCatchupString);
        writer->revive(newSocketFd);
        socketFd = newSocketFd;
        writer->unlock();
        break;
      } catch (const runtime_error& err) {
        VLOG(1) << "Failed while recovering" << endl;
        socketHandler->close(newSocketFd);
        writer->unlock();
      }
    } else {
      VLOG(1) << "Waiting to retry...";
      sleep(1);
    }
  }
}
