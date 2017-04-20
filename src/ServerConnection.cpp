#include "ServerConnection.hpp"
namespace et {
ServerConnection::ServerConnection(
    std::shared_ptr<SocketHandler> _socketHandler, int _port,
    shared_ptr<ServerConnectionHandler> _serverHandler)
    : socketHandler(_socketHandler),
      port(_port),
      serverHandler(_serverHandler),
      stop(false)
{
}

ServerConnection::~ServerConnection() {}

void ServerConnection::run() {
  while (!stop) {
    VLOG_EVERY_N(2, 30) << "Listening for connection" << endl;
    int clientSocketFd = socketHandler->listen(port);
    if (clientSocketFd < 0) {
      sleep(1);
      continue;
    }
    VLOG(1) << "SERVER: got client socket fd: " << clientSocketFd << endl;
    clientHandler(clientSocketFd);
  }
}

void ServerConnection::close() {
  stop = true;
  socketHandler->stopListening();
  for (const auto& it : clientConnections) {
    it.second->closeSocket();
  }
  clientConnections.clear();
}

void ServerConnection::clientHandler(int clientSocketFd) {
  string clientId;
  try {
    et::ConnectRequest request =
        socketHandler->readProto<et::ConnectRequest>(clientSocketFd, true);
    {
      int version = request.version();
      if (version != PROTOCOL_VERSION) {
        LOG(ERROR) << "Got a client request but the client version does not match.  Client: " <<
            version << " != Server: " << PROTOCOL_VERSION;
        et::ConnectResponse response;

        std::ostringstream errorStream;
        errorStream << "Mismatched protocol versions.  Client: "
                    << request.version() << " != Server: " << PROTOCOL_VERSION;
        response.set_status(MISMATCHED_PROTOCOL);
        response.set_error(errorStream.str());
        socketHandler->writeProto(clientSocketFd, response, true);
        socketHandler->close(clientSocketFd);
        return;
      }
    }
    clientId = request.clientid();
    LOG(INFO) << "Got client with id: " << clientId << endl;
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
      newClientConnection(clientId, clientSocketFd);
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
}

void ServerConnection::newClientConnection(
    const string& clientId,
    int socketFd) {
  VLOG(1) << "Created client with id " << clientId << endl;

  shared_ptr<ServerClientConnection> scc(
      new ServerClientConnection(socketHandler, clientId, socketFd, clientKeys[clientId]));
  clientConnections.insert(std::make_pair(clientId, scc));
}

bool ServerConnection::removeClient(
    const string &id) {
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
}
