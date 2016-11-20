#include "ServerConnection.hpp"

ServerConnection::ServerConnection(
  std::shared_ptr<SocketHandler> _socketHandler,
  int _port
  ) :
  socketHandler(_socketHandler),
  port(_port) {
}

void ServerConnection::run() {
  while(true) {
    int clientSocketFd = socketHandler->listen(port);
    if (clientSocketFd < 0) {
      break;
    }
    int clientId;
    socketHandler->read(clientSocketFd,&clientId,sizeof(int));
    //TODO: I think we need to spin off a thread/process to listen to this client
    if (clientId == -1) {
      newClient(clientSocketFd);
    } else {
      if (!clientExists(clientId)) {
        throw new std::runtime_error("Tried to revive an unknown client");
      }
      recoverClient(clientId, clientSocketFd);
    }
  }
}

int ServerConnection::newClient(int socketFd) {
  int clientId = rand();
  while (clientExists(clientId)) {
    clientId++;
    if (clientId<0) {
      throw new std::runtime_error("Ran out of client ids");
    }
  }

  socketHandler->writeAll(socketFd, &clientId, sizeof(int));
  clients.insert(std::make_pair(clientId, ClientState(socketHandler,clientId,socketFd)));
  return clientId;
}

bool ServerConnection::recoverClient(int clientId, int newSocketFd) {
  std::shared_ptr<BackedReader> reader = clients.find(clientId)->second.reader;
  std::shared_ptr<BackedWriter> writer = clients.find(clientId)->second.writer;

  // TODO: Merge with identical logic in ClientConnection.cpp
  int64_t localReaderSequenceNumber = reader->getSequenceNumber();
  socketHandler->writeAll(newSocketFd, &localReaderSequenceNumber, sizeof(int64_t));
  int64_t remoteReaderSequenceNumber;
  socketHandler->readAll(newSocketFd, &remoteReaderSequenceNumber, sizeof(int64_t));

  std::string writerCatchupString = writer->recover(remoteReaderSequenceNumber);
  int64_t writerCatchupStringLength = writerCatchupString.length();
  socketHandler->writeAll(newSocketFd, &writerCatchupStringLength, sizeof(int64_t));
  socketHandler->writeAll(newSocketFd, &writerCatchupString[0], writerCatchupString.length());

  int64_t readerCatchupBytes;
  socketHandler->readAll(newSocketFd, &readerCatchupBytes, sizeof(int64_t));
  std::string readerCatchupString(readerCatchupBytes, (char)0);
  socketHandler->readAll(newSocketFd, &readerCatchupString[0], readerCatchupBytes);

  // fix revive
  clients.find(clientId)->second.revive(newSocketFd, readerCatchupString);
  return true;
}
