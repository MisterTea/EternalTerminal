#include "ServerConnection.hpp"

ServerConnection::ServerConnection(
  std::shared_ptr<SocketHandler> _socketHandler,
  int _port,
  shared_ptr<ServerConnectionHandler> _serverHandler,
  const string& _key
  ) :
  socketHandler(_socketHandler),
  port(_port),
  serverHandler(_serverHandler),
  stop(false),
  key(_key) {
}

ServerConnection::~ServerConnection() {
  if(clientConnectThread) {
    clientConnectThread->join();
    clientConnectThread.reset();
  }
}

void ServerConnection::run() {
  while(!stop) {
    VLOG(1) << "Listening for connection" << endl;
    int clientSocketFd = socketHandler->listen(port);
    if (clientSocketFd < 0) {
      sleep(1);
      continue;
    }
    VLOG(1) << "SERVER: got client socket fd: " << clientSocketFd << endl;
    if (clientConnectThread) {
      clientConnectThread->join();
    }
    clientConnectThread = shared_ptr<thread>(
      new thread(&ServerConnection::clientHandler, this, clientSocketFd));
  }
}

void ServerConnection::close() {
  stop=true;
  socketHandler->stopListening();
}

void ServerConnection::clientHandler(int clientSocketFd) {
  int clientId;
  try {
    socketHandler->readAllTimeout(clientSocketFd,&clientId,sizeof(int));
    if (clientId == -1) {
      clientId = newClient(clientSocketFd);
      shared_ptr<ServerClientConnection> serverClientState = getClient(clientId);
      if(serverHandler && !serverHandler->newClient(serverClientState)) {
        // Destroy the new client
        removeClient(clientId);
        socketHandler->close(clientSocketFd);
      }
    } else {
      if (!clientExists(clientId)) {
        throw std::runtime_error("Tried to revive an unknown client");
      }
      recoverClient(clientId, clientSocketFd);
    }
  } catch (const runtime_error& err) {
    // Comm failed, close the connection
    socketHandler->close(clientSocketFd);
  }
}

int ServerConnection::newClient(int socketFd) {
  int clientId = rand();
  while (clientExists(clientId)) {
    clientId++;
    if (clientId<0) {
      throw std::runtime_error("Ran out of client ids");
    }
  }
  VLOG(1) << "Created client with id " << clientId << endl;

  socketHandler->writeAllTimeout(socketFd, &clientId, sizeof(int));
  shared_ptr<ServerClientConnection> scc(
    new ServerClientConnection(socketHandler,clientId,socketFd,key));
  clients.insert(std::make_pair(clientId, scc));
  return clientId;
}

bool ServerConnection::recoverClient(int clientId, int newSocketFd) {
  std::shared_ptr<BackedReader> reader = clients.find(clientId)->second->getReader();
  std::shared_ptr<BackedWriter> writer = clients.find(clientId)->second->getWriter();

  // If we didn't know the client disconnected, we do now.  Invalidate the old client.
  int socket = writer->getSocketFd();
  if (socket != -1) {
    VLOG(1) << "Invalidating socket" << endl;
    writer->invalidateSocket();
    reader->invalidateSocket();
    socketHandler->close(socket);
  }

  // TODO: Merge with identical logic in ClientConnection.cpp
  int64_t localReaderSequenceNumber = reader->getSequenceNumber();
  socketHandler->writeAllTimeout(newSocketFd, &localReaderSequenceNumber, sizeof(int64_t));
  int64_t remoteReaderSequenceNumber;
  socketHandler->readAllTimeout(newSocketFd, &remoteReaderSequenceNumber, sizeof(int64_t));

  std::string writerCatchupString = writer->recover(remoteReaderSequenceNumber);
  try {
    int64_t writerCatchupStringLength = writerCatchupString.length();
    socketHandler->writeAllTimeout(newSocketFd, &writerCatchupStringLength, sizeof(int64_t));
    socketHandler->writeAllTimeout(newSocketFd, &writerCatchupString[0], writerCatchupString.length());

    int64_t readerCatchupBytes;
    socketHandler->readAllTimeout(newSocketFd, &readerCatchupBytes, sizeof(int64_t));
    std::string readerCatchupString(readerCatchupBytes, (char)0);
    socketHandler->readAllTimeout(newSocketFd, &readerCatchupString[0], readerCatchupBytes);

    clients.find(clientId)->second->revive(newSocketFd, readerCatchupString);
  } catch (const runtime_error& err) {
  }
  writer->unlock();
  return true;
}
