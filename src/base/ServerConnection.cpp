#include "ServerConnection.hpp"
namespace et {
ServerConnection::ServerConnection(
    std::shared_ptr<SocketHandler> _socketHandler,
    const SocketEndpoint& _serverEndpoint)
    : socketHandler(_socketHandler),
      serverEndpoint(_serverEndpoint),
      clientHandlerThreadPool(new ThreadPool(8)) {
  socketHandler->listen(serverEndpoint);
}

ServerConnection::~ServerConnection() {}

bool ServerConnection::acceptNewConnection(int fd) {
  // Loop through existing threads, killing the ones that are done
  VLOG(1) << "Accepting connection";
  int clientSocketFd = socketHandler->accept(fd);
  if (clientSocketFd < 0) {
    return false;
  }
  VLOG(1) << "SERVER: got client socket fd: " << clientSocketFd;
  lock_guard<std::recursive_mutex> guard(classMutex);
  clientHandlerThreadPool->enqueue(
      [this, clientSocketFd]() { this->clientHandler(clientSocketFd); });
  return true;
}

void ServerConnection::shutdown() {
  lock_guard<std::recursive_mutex> guard(classMutex);
  socketHandler->stopListening(serverEndpoint);
  clientHandlerThreadPool.reset();
  for (const auto& it : clientConnections) {
    it.second->shutdown();
  }
  clientConnections.clear();
}

void ServerConnection::clientHandler(int clientSocketFd) {
  el::Helpers::setThreadName("server-clientHandler");

  string clientId;
  bool createdClientConnection = false;
  try {
    et::ConnectRequest request =
        socketHandler->readProto<et::ConnectRequest>(clientSocketFd, true);
    {
      int version = request.version();
      if (version != PROTOCOL_VERSION) {
        STERROR << "Got a client request but the client version does not "
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
        return;
      }
    }
    clientId = request.clientid();
    shared_ptr<ServerClientConnection> serverClientState = NULL;
    bool clientKeyExistsNow;

    {
      lock_guard<std::recursive_mutex> guard(classMutex);

      // Log within the mutex, so that we can guarantee that this client id wins
      // the lock when this message appears.
      LOG(INFO) << "Got client with id: " << clientId;

      clientKeyExistsNow = clientKeyExists(clientId);
      if (clientConnectionExists(clientId)) {
        serverClientState = getClientConnection(clientId);
      } else if (clientKeyExistsNow) {
        createdClientConnection = true;
        serverClientState.reset(new ServerClientConnection(
            socketHandler, clientId, clientSocketFd, clientKeys.at(clientId)));
        clientConnections.insert(std::make_pair(clientId, serverClientState));
      }
    }
    if (!clientKeyExistsNow) {
      LOG(INFO) << "Got a client that we have no key for";

      et::ConnectResponse response;
      std::ostringstream errorStream;
      errorStream << "Client is not registered";
      response.set_error(errorStream.str());
      response.set_status(INVALID_KEY);
      socketHandler->writeProto(clientSocketFd, response, true);

      socketHandler->close(clientSocketFd);
    } else if (createdClientConnection) {
      et::ConnectResponse response;
      response.set_status(NEW_CLIENT);
      socketHandler->writeProto(clientSocketFd, response, true);

      LOG(INFO) << "New client.  Setting up connection";
      VLOG(1) << "Created client with id " << clientId;

      {
        lock_guard<std::recursive_mutex> guard(classMutex);

        if (!newClient(serverClientState)) {
          VLOG(1) << "newClient failed";
          // Client creation failed, Destroy the new client
          removeClient(clientId);
          socketHandler->close(clientSocketFd);
        }
      }
    } else {
      et::ConnectResponse response;
      response.set_status(RETURNING_CLIENT);
      socketHandler->writeProto(clientSocketFd, response, true);

      lock_guard<std::recursive_mutex> guard(classMutex);
      serverClientState->recoverClient(clientSocketFd);
    }
  } catch (const runtime_error& err) {
    // Comm failed, close the connection
    LOG(WARNING) << "Error handling new client: " << err.what();
    if (createdClientConnection) {
      destroyPartialConnection(clientId);
    }
    socketHandler->close(clientSocketFd);
  } catch (const std::exception& e) {
    LOG(ERROR) << "Got an unexpected error handling new client: " << e.what();
    if (createdClientConnection) {
      destroyPartialConnection(clientId);
    }
    socketHandler->close(clientSocketFd);
  }
}

bool ServerConnection::removeClient(const string& id) {
  lock_guard<std::recursive_mutex> guard(classMutex);
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

void ServerConnection::destroyPartialConnection(const string& clientId) {
  lock_guard<std::recursive_mutex> guard(classMutex);
  const auto it = clientConnections.find(clientId);
  if (it == clientConnections.end()) {
    return;
  }
  it->second->shutdown();
  clientConnections.erase(it);
}

}  // namespace et
