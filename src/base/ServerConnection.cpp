#include "ServerConnection.hpp"
namespace et {
ServerConnection::ServerConnection(
    std::shared_ptr<SocketHandler> _socketHandler,
    const SocketEndpoint& _serverEndpoint)
    : socketHandler(_socketHandler),
      serverEndpoint(_serverEndpoint),
      startTime_(time(NULL)),
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
  {
    lock_guard<std::recursive_mutex> guard(classMutex);
    socketHandler->stopListening(serverEndpoint);
  }
  // In-flight clientHandlers take classMutex, so join the pool without it.
  clientHandlerThreadPool.reset();
  lock_guard<std::recursive_mutex> guard(classMutex);
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
    et::ConnectRequest request = socketHandler->readProto<et::ConnectRequest>(
        clientSocketFd, true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
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
    bool clientWasRemoved;
    string clientKey;

    {
      lock_guard<std::recursive_mutex> guard(classMutex);

      // Log within the mutex, so that we can guarantee that this client id wins
      // the lock when this message appears.
      LOG(INFO) << "Got client with id: " << clientId;

      clientKeyExistsNow = clientKeyExists(clientId);
      if (clientKeyExistsNow) {
        clientKey = clientKeys.at(clientId);
      }
      pruneRemovedClientIds(time(NULL));
      clientWasRemoved =
          removedClientIds.find(clientId) != removedClientIds.end();
    }

    // Legacy handshake for peers without the challenge capability. Remove at
    // the next PROTOCOL_VERSION bump.
    const bool legacyPeer = !request.supportschallenge();
    bool authenticated = false;
    string authChallenge;
    const bool resetIntent = !legacyPeer && request.resetintent();
    if (clientKeyExistsNow) {
      authenticated =
          legacyPeer ||
          authenticateClient(clientSocketFd, clientId, clientKey,
                             request.version(), resetIntent, &authChallenge);
    }

    {
      lock_guard<std::recursive_mutex> guard(classMutex);
      // The key may have been removed or replaced during the challenge.
      auto keyIt = clientKeys.find(clientId);
      if (keyIt == clientKeys.end() || keyIt->second != clientKey) {
        authenticated = false;
        clientKeyExistsNow = false;
      }
      if (authenticated && clientConnectionExists(clientId)) {
        serverClientState = getClientConnection(clientId);
      } else if (authenticated) {
        createdClientConnection = true;
        serverClientState.reset(new ServerClientConnection(
            socketHandler, clientId, clientSocketFd, clientKey));
        clientConnections.insert(std::make_pair(clientId, serverClientState));
      }
    }
    if (!clientKeyExistsNow) {
      LOG(INFO) << "Got a client that we have no key for";

      et::ConnectResponse response;
      std::ostringstream errorStream;
      errorStream << "Client is not registered";
      response.set_error(errorStream.str());
      // Right after a restart, terminals may not have re-registered yet.
      if (!legacyPeer && !clientWasRemoved &&
          time(NULL) - startTime_ < recoveryGraceSeconds) {
        LOG(INFO) << "Within the recovery grace window; asking client "
                  << clientId << " to retry.";
        response.set_status(RETRY_LATER);
      } else {
        response.set_status(INVALID_KEY);
      }
      socketHandler->writeProto(clientSocketFd, response, true);

      socketHandler->close(clientSocketFd);
    } else if (!authenticated) {
      LOG(WARNING) << "Rejecting client with invalid authentication proof";
      et::ConnectResponse response;
      response.set_status(INVALID_KEY);
      response.set_error("Client authentication failed");
      socketHandler->writeProto(clientSocketFd, response, true);
      socketHandler->close(clientSocketFd);
    } else if (createdClientConnection) {
      // A known key with no connection is either a new session or a terminal
      // that re-registered after an etserver restart.
      const bool resume = shouldResumeAsReturning(clientId);
      if (legacyPeer && resume) {
        // Legacy peers can't reset; answer as a pre-restart-recovery server.
        LOG(INFO) << "Legacy client " << clientId
                  << " cannot resume after restart";
        et::ConnectResponse response;
        response.set_status(INVALID_KEY);
        response.set_error("Client is not registered");
        socketHandler->writeProto(clientSocketFd, response, true);
        // The partial connection owns clientSocketFd and closes it.
        destroyPartialConnection(clientId);
        return;
      }
      const string resetSalt =
          resume ? CryptoHandler::randomBytes(CryptoHandler::EPOCH_SALT_BYTES)
                 : string();
      const ConnectStatus status = resume ? RETURNING_CLIENT : NEW_CLIENT;
      et::ConnectResponse response;
      response.set_status(status);
      if (!legacyPeer) {
        response.set_resetrequired(resume);
        response.set_resetsalt(resetSalt);
        response.set_resetproof(CryptoHandler::resetDecisionProof(
            clientKey, clientId, PROTOCOL_VERSION, authChallenge, status,
            resume, resetSalt));
      }
      socketHandler->writeProto(clientSocketFd, response, true);

      if (resume) {
        LOG(INFO) << "Resuming existing session for " << clientId;
        if (!serverClientState->recoverClient(clientSocketFd,
                                              /*forceReset=*/true, resetSalt)) {
          LOG(WARNING) << "Resume handshake failed for " << clientId;
          // Keep the key: the terminal is still live.
          destroyPartialConnection(clientId);
        } else {
          resumeClient(serverClientState);
        }
      } else {
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
      }
    } else {
      const string resetSalt =
          resetIntent
              ? CryptoHandler::randomBytes(CryptoHandler::EPOCH_SALT_BYTES)
              : string();
      et::ConnectResponse response;
      response.set_status(RETURNING_CLIENT);
      if (!legacyPeer) {
        response.set_resetrequired(resetIntent);
        response.set_resetsalt(resetSalt);
        response.set_resetproof(CryptoHandler::resetDecisionProof(
            clientKey, clientId, PROTOCOL_VERSION, authChallenge,
            RETURNING_CLIENT, resetIntent, resetSalt));
      }
      socketHandler->writeProto(clientSocketFd, response, true);

      // Deliberately not under classMutex: recover() blocks on socket I/O for
      // as long as the socket timeout allows, and holding a server-wide lock
      // across that stalls the accept loop until the reconnect gives up.
      // serverClientState keeps the connection alive, and recoverClient()
      // serializes concurrent reconnects on the connection's own mutex.
      serverClientState->recoverClient(clientSocketFd, resetIntent, resetSalt);
    }
  } catch (const runtime_error& err) {
    // Comm failed, close the connection
    LOG(WARNING) << "Error handling new client: " << err.what();
    if (createdClientConnection) {
      destroyPartialConnection(clientId);
    } else {
      socketHandler->close(clientSocketFd);
    }
  } catch (const std::exception& e) {
    LOG(ERROR) << "Got an unexpected error handling new client: " << e.what();
    if (createdClientConnection) {
      destroyPartialConnection(clientId);
    } else {
      socketHandler->close(clientSocketFd);
    }
  }
}

bool ServerConnection::authenticateClient(int clientSocketFd,
                                          const string& clientId,
                                          const string& clientKey,
                                          int protocolVersion, bool resetIntent,
                                          string* challengeOut) {
  const string challenge =
      CryptoHandler::randomBytes(CryptoHandler::AUTH_CHALLENGE_BYTES);
  et::ConnectResponse challengeResponse;
  challengeResponse.set_authchallenge(challenge);
  socketHandler->writeProto(clientSocketFd, challengeResponse, true);

  const et::ConnectAuth auth = socketHandler->readProto<et::ConnectAuth>(
      clientSocketFd, true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
  const bool verified = CryptoHandler::verifyConnectionProof(
      auth.proof(), clientKey, clientId, protocolVersion, challenge,
      resetIntent);
  if (verified) {
    *challengeOut = challenge;
  }
  return verified;
}

bool ServerConnection::removeClient(const string& id) {
  shared_ptr<ServerClientConnection> connection;
  {
    lock_guard<std::recursive_mutex> guard(classMutex);
    if (clientKeys.find(id) == clientKeys.end()) {
      return false;
    }
    const time_t now = time(NULL);
    pruneRemovedClientIds(now);
    removedClientIds[id] = now;
    clientKeys.erase(id);
    const auto it = clientConnections.find(id);
    if (it == clientConnections.end()) {
      return true;
    }
    connection = it->second;
    clientConnections.erase(it);
  }
  // Outside classMutex: shutdown() waits on the connection mutex, which a
  // reconnect can hold across blocking socket I/O.
  connection->shutdown();
  return true;
}

void ServerConnection::pruneRemovedClientIds(time_t now) {
  for (auto it = removedClientIds.begin(); it != removedClientIds.end();) {
    if (now >= it->second &&
        now - it->second > static_cast<time_t>(recoveryGraceSeconds)) {
      it = removedClientIds.erase(it);
    } else {
      ++it;
    }
  }
}

void ServerConnection::destroyPartialConnection(const string& clientId) {
  shared_ptr<ServerClientConnection> connection;
  {
    lock_guard<std::recursive_mutex> guard(classMutex);
    const auto it = clientConnections.find(clientId);
    if (it == clientConnections.end()) {
      return;
    }
    connection = it->second;
    clientConnections.erase(it);
  }
  connection->shutdown();
}

}  // namespace et
