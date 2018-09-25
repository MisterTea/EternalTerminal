#include "ServerConnection.hpp"
namespace et {
ServerConnection::ServerConnection(
    std::shared_ptr<SocketHandler> _socketHandler,
    const SocketEndpoint& _serverEndpoint,
    shared_ptr<ServerConnectionHandler> _serverHandler)
    : socketHandler(_socketHandler),
      serverEndpoint(_serverEndpoint),
      serverHandler(_serverHandler) {
  socketHandler->listen(serverEndpoint);
}

ServerConnection::~ServerConnection() {}

void ServerConnection::acceptNewConnection(int fd) {
  // Loop through existing threads, killing the ones that are done
  if (clientHandlerThreads.size()) {
    VLOG(1) << "Reaping connect threads...";
    int numReaped = 0;
    for (int a = int(clientHandlerThreads.size()) - 1; a >= 0; a--) {
      if (clientHandlerThreads[a]->done) {
        clientHandlerThreads[a]->t->join();
        clientHandlerThreads.erase(clientHandlerThreads.begin() + a);
        numReaped++;
      }
    }
    VLOG(1) << "Reaped " << numReaped << " connect threads";
  }

  VLOG(1) << "Accepting connection";
  int clientSocketFd = socketHandler->accept(fd);
  if (clientSocketFd < 0) {
    return;
  }
  VLOG(1) << "SERVER: got client socket fd: " << clientSocketFd;
  auto threadWrapper =
      shared_ptr<TerminationRecordingThread>(new TerminationRecordingThread());
  auto clientHandlerThread = std::shared_ptr<std::thread>(
      new thread(&ServerConnection::clientHandler, this, clientSocketFd,
                 &(threadWrapper->done)));
  threadWrapper->t = clientHandlerThread;
  clientHandlerThreads.push_back(threadWrapper);
}

void ServerConnection::close() {
  socketHandler->stopListening(serverEndpoint);
  for (const auto& it : clientConnections) {
    it.second->closeSocket();
  }
  clientConnections.clear();
}

void ServerConnection::clientHandler(int clientSocketFd, atomic<bool>* done) {
  // Only one thread can be connecting at a time, the rest have to wait.
  // TODO: Relax this constraint once we are confident that it works as-is.
  // lock_guard<std::mutex> guard(connectMutex);
  el::Helpers::setThreadName("server-clientHandler");

  string clientId;
  try {
    et::ConnectRequest request =
        socketHandler->readProto<et::ConnectRequest>(clientSocketFd, true);
    {
      int version = request.version();
      if (version != PROTOCOL_VERSION) {
        LOG(ERROR) << "Got a client request but the client version does not "
                      "match.  Client: "
                   << version << " != Server: " << PROTOCOL_VERSION;
        et::ConnectResponse response;

        std::ostringstream errorStream;
        errorStream
            << "Mismatched protocol versions.  "
            << "Your client & server must be on the same version of ET.  "
            << "Client: " << request.version()
            << " != Server: " << PROTOCOL_VERSION;
        response.set_status(MISMATCHED_PROTOCOL);
        response.set_error(errorStream.str());
        socketHandler->writeProto(clientSocketFd, response, true);
        socketHandler->close(clientSocketFd);
        *done = true;
        return;
      }
    }
    clientId = request.clientid();
    LOG(INFO) << "Got client with id: " << clientId;
    if (!clientKeyExists(clientId)) {
      LOG(ERROR) << "Got a client that we have no key for";

      et::ConnectResponse response;
      std::ostringstream errorStream;
      errorStream << "Client is not registered";
      response.set_error(errorStream.str());
      response.set_status(INVALID_KEY);
      socketHandler->writeProto(clientSocketFd, response, true);

      socketHandler->close(clientSocketFd);
    } else if (!clientConnectionExists(clientId)) {
      et::ConnectResponse response;
      response.set_status(NEW_CLIENT);
      socketHandler->writeProto(clientSocketFd, response, true);

      LOG(INFO) << "New client.  Setting up connection";
      VLOG(1) << "Created client with id " << clientId;

      shared_ptr<ServerClientConnection> scc(new ServerClientConnection(
          socketHandler, clientId, clientSocketFd, clientKeys[clientId]));
      {
        lock_guard<std::recursive_mutex> guard(classMutex);
        clientConnections.insert(std::make_pair(clientId, scc));
      }

      shared_ptr<ServerClientConnection> serverClientState =
          getClientConnection(clientId);
      if (serverHandler && !serverHandler->newClient(serverClientState)) {
        // Client creation failed, Destroy the new client
        removeClient(clientId);
        socketHandler->close(clientSocketFd);
      }
    } else {
      et::ConnectResponse response;
      response.set_status(RETURNING_CLIENT);
      socketHandler->writeProto(clientSocketFd, response, true);

      shared_ptr<ServerClientConnection> serverClientState =
          getClientConnection(clientId);
      serverClientState->recoverClient(clientSocketFd);
    }
  } catch (const runtime_error& err) {
    // Comm failed, close the connection
    LOG(ERROR) << "Error handling new client: " << err.what();
    socketHandler->close(clientSocketFd);
  }
  *done = true;
}

bool ServerConnection::removeClient(const string& id) {
  if (clientKeys.find(id) == clientKeys.end()) {
    return false;
  }
  clientKeys.erase(id);
  if (clientConnections.find(id) == clientConnections.end()) {
    return true;
  }
  auto connection = clientConnections[id];
  connection->shutdown();
  clientConnections.erase(id);
  return true;
}
}  // namespace et
