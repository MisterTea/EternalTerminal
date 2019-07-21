#include "ServerConnection.hpp"

#include "MessageReader.hpp"
#include "MessageWriter.hpp"

namespace et {
ServerConnection::ServerConnection(
    std::shared_ptr<SocketHandler> _socketHandler,
    const SocketEndpoint& _serverEndpoint)
    : socketHandler(_socketHandler),
      serverEndpoint(_serverEndpoint),
      clientHandlerThreadPool(8) {
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
  clientHandlerThreadPool.push(
      [this, clientSocketFd](int id) { this->clientHandler(clientSocketFd); });
  return true;
}

void ServerConnection::shutdown() {
  socketHandler->stopListening(serverEndpoint);
  clientHandlerThreadPool.stop();
  for (const auto& it : clientConnections) {
    it.second->shutdown();
  }
  clientConnections.clear();
}

void ServerConnection::clientHandler(int clientSocketFd) {
  el::Helpers::setThreadName("server-clientHandler");

  string clientId;
  try {
    string request = socketHandler->readString(socketFd, true);
    string requestId;
    int requestVersion;
    {
      MessageReader reader(request);
      requestId = reader.readPrimitive<string>();
      requestVersion = reader.readPrimitive<int>();
    }
    {
      int version = requestVersion;
      if (version != PROTOCOL_VERSION) {
        LOG(ERROR) << "Got a client request but the client version does not "
                      "match.  Client: "
                   << version << " != Server: " << PROTOCOL_VERSION;
        string response;

        std::ostringstream errorStream;
        errorStream
            << "Mismatched protocol versions.  "
            << "Your client & server must be on the same version of ET.  "
            << "Client: " << requestVersion
            << " != Server: " << PROTOCOL_VERSION;
        {
          MessageWriter writer;
          writer.writePrimitive(int(MISMATCHED_PROTOCOL));
          writer.writePrimitive(errorStream.str());
          response = writer.finish();
        }
        socketHandler->writeString(clientSocketFd, response, true);
        socketHandler->close(clientSocketFd);
        return;
      }
    }
    clientId = requestId;
    LOG(INFO) << "Got client with id: " << clientId;
    shared_ptr<ServerClientConnection> serverClientState = NULL;
    bool clientKeyExistsNow;
    {
      lock_guard<std::recursive_mutex> guard(classMutex);
      clientKeyExistsNow = clientKeyExists(clientId);
      if (clientConnectionExists(clientId)) {
        serverClientState = getClientConnection(clientId);
      }
    }
    if (!clientKeyExistsNow) {
      LOG(ERROR) << "Got a client that we have no key for";

      string response;
      std::ostringstream errorStream;
      errorStream << "Client is not registered";
      {
        MessageWriter writer;
        writer.writePrimitive(int(INVALID_KEY));
        writer.writePrimitive(errorStream.str());
        response = writer.finish();
      }
      socketHandler->writeString(clientSocketFd, response, true);

      socketHandler->close(clientSocketFd);
    } else if (serverClientState.get() == NULL) {
      string response;
      {
        MessageWriter writer;
        writer.writePrimitive(int(NEW_CLIENT));
        writer.writePrimitive(string("");
        response = writer.finish();
      }
      socketHandler->writeString(clientSocketFd, response, true);

      LOG(INFO) << "New client.  Setting up connection";
      VLOG(1) << "Created client with id " << clientId;

      {
        lock_guard<std::recursive_mutex> guard(classMutex);
        serverClientState.reset(new ServerClientConnection(
            socketHandler, clientId, clientSocketFd, clientKeys[clientId]));
        clientConnections.insert(std::make_pair(clientId, serverClientState));

        if (!newClient(serverClientState)) {
          VLOG(1) << "newClient failed";
          // Client creation failed, Destroy the new client
          removeClient(clientId);
          socketHandler->close(clientSocketFd);
        }
      }
    } else {
      string response;
      {
        MessageWriter writer;
        writer.writePrimitive(int(RETURNING_CLIENT));
        writer.writePrimitive(string(""));
        response = writer.finish();
      }
      socketHandler->writeString(clientSocketFd, response, true);

      lock_guard<std::recursive_mutex> guard(classMutex);
      serverClientState->recoverClient(clientSocketFd);
    }
  } catch (const runtime_error& err) {
    // Comm failed, close the connection
    LOG(ERROR) << "Error handling new client: " << err.what();
    socketHandler->close(clientSocketFd);
  }
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
