#include "ClientConnection.hpp"

namespace et {
const string ClientConnection::LEGACY_SERVER_REATTACH_ERROR =
    "Server does not support session reattach; upgrade etserver";

ClientConnection::ClientConnection(
    std::shared_ptr<SocketHandler> _socketHandler,
    const SocketEndpoint& _remoteEndpoint, const string& _id,
    const string& _key, bool _resetIntent)
    : Connection(_socketHandler, _id, _key),
      remoteEndpoint(_remoteEndpoint),
      resetOnConnect(_resetIntent) {}

ClientConnection::~ClientConnection() {
  if (reconnectThread) {
    reconnectThread->join();
    reconnectThread.reset();
  }
  // Close the socket without spawning a reconnect thread
  closeSocket();
}

bool ClientConnection::connect() {
  try {
    VLOG(1) << "Connecting";
    socketFd = socketHandler->connect(remoteEndpoint);
    if (socketFd == -1) {
      VLOG(1) << "Could not connect to host";
      return false;
    }
    VLOG(1) << "Sending id";
    et::ConnectResponse response;
    connectHandshake(socketFd, &response, resetOnConnect);
    lastStatus_.store(response.status());
    if (response.status() != NEW_CLIENT &&
        response.status() != RETURNING_CLIENT) {
      // Note: the response can be returning client if the client died while
      // performing the initial connection but the server thought the client
      // survived.
      STERROR << "Error connecting to server: " << response.status() << ": "
              << response.error();
      CLOG(INFO, "stdout") << "Error connecting to server: "
                           << response.status() << ": " << response.error()
                           << endl;
      string s = string("Error connecting to server: ") +
                 to_string(response.status()) + string(": ") + response.error();
      throw std::runtime_error(s.c_str());
    }
    VLOG(1) << "Creating backed reader";
    reader = std::shared_ptr<BackedReader>(
        new BackedReader(socketHandler,
                         shared_ptr<CryptoHandler>(
                             new CryptoHandler(key, SERVER_CLIENT_NONCE_MSB)),
                         socketFd));
    VLOG(1) << "Creating backed writer";
    writer = std::shared_ptr<BackedWriter>(
        new BackedWriter(socketHandler,
                         shared_ptr<CryptoHandler>(
                             new CryptoHandler(key, CLIENT_SERVER_NONCE_MSB)),
                         socketFd));
    if (response.status() == RETURNING_CLIENT && response.resetrequired()) {
      // This process has no sequence history for the server's session, so
      // both sides restart at sequence 0 and the caller skips bootstrap.
      VLOG(1) << "Returning client: performing reset recovery";
      if (!recover(socketFd, response.resetrequired(), response.resetsalt())) {
        LOG(WARNING) << "Reset recovery failed during connect";
        return false;
      }
      recovered_.store(true);
    }
    VLOG(1) << "Client Connection established";
    return true;
  } catch (const runtime_error& err) {
    LOG(INFO) << "Got failure during connect";
    if (socketFd != -1) {
      // socketHandler->close() would leave socketFd set and double-close it.
      closeSocket();
    }
    if (err.what() == LEGACY_SERVER_REATTACH_ERROR) {
      throw;
    }
  }
  return false;
}

void ClientConnection::closeSocketAndMaybeReconnect() {
  waitReconnect();
  LOG(INFO) << "Closing socket";
  closeSocket();
  if (!isShuttingDown()) {
    LOG(INFO) << "Socket closed, starting new reconnect thread";
    reconnectThread = std::shared_ptr<std::thread>(
        new std::thread(&ClientConnection::pollReconnect, this));
  }
}

void ClientConnection::waitReconnect() {
  if (reconnectThread.get()) {
    LOG(INFO) << "Waiting for reconnect thread to finish";
    reconnectThread->join();
    reconnectThread.reset();
  }
}

void ClientConnection::connectHandshake(int fd, et::ConnectResponse* response,
                                        bool resetIntent) {
  et::ConnectRequest request;
  request.set_clientid(id);
  request.set_version(PROTOCOL_VERSION);
  request.set_resetintent(resetIntent);
  request.set_supportschallenge(true);
  socketHandler->writeProto(fd, request, true);

  // A restarting server can accept into its backlog and never answer.
  bool responded = false;
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (socketHandler->hasData(fd)) {
      responded = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!responded) {
    throw std::runtime_error("Server did not answer the connect handshake");
  }

  et::ConnectResponse challengeOrResponse =
      socketHandler->readProto<et::ConnectResponse>(
          fd, true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
  if (!challengeOrResponse.has_authchallenge()) {
    // Legacy handshake for peers without the challenge capability. Remove at
    // the next PROTOCOL_VERSION bump.
    if (resetIntent && challengeOrResponse.status() == RETURNING_CLIENT) {
      throw std::runtime_error(LEGACY_SERVER_REATTACH_ERROR);
    }
    *response = challengeOrResponse;
    response->clear_resetrequired();
    response->clear_resetsalt();
    response->clear_resetproof();
    return;
  }
  if (challengeOrResponse.authchallenge().size() !=
      CryptoHandler::AUTH_CHALLENGE_BYTES) {
    throw std::runtime_error("Server sent an invalid authentication challenge");
  }

  et::ConnectAuth auth;
  auth.set_proof(CryptoHandler::connectionProof(
      key, id, PROTOCOL_VERSION, challengeOrResponse.authchallenge(),
      resetIntent));
  socketHandler->writeProto(fd, auth, true);
  *response = socketHandler->readProto<et::ConnectResponse>(
      fd, true, SocketHandler::MAX_HANDSHAKE_PROTO_LENGTH);
  if (response->status() == NEW_CLIENT ||
      response->status() == RETURNING_CLIENT) {
    if (response->resetrequired() &&
        response->resetsalt().size() != CryptoHandler::EPOCH_SALT_BYTES) {
      throw std::runtime_error("Invalid reset decision salt");
    }
    if (!response->has_resetproof() ||
        !CryptoHandler::verifyResetDecisionProof(
            response->resetproof(), key, id, PROTOCOL_VERSION,
            challengeOrResponse.authchallenge(), response->status(),
            response->resetrequired(), response->resetsalt())) {
      throw std::runtime_error(
          "Connect success is missing a valid reset decision proof");
    }
  }
}

void ClientConnection::pollReconnect() {
  el::Helpers::setThreadName("Reconnect");
  LOG(INFO) << "Trying to reconnect to " << remoteEndpoint << endl;
  while (true) {
    {
      lock_guard<std::recursive_mutex> guard(connectionMutex);
      if (socketFd != -1) {
        break;
      }
      if (shuttingDown) {
        LOG(INFO) << "Aborting reconnect loop because shutdown was called";
        return;
      }
      LOG_EVERY_N(10, INFO) << "In reconnect loop " << remoteEndpoint << endl;
      int newSocketFd = socketHandler->connect(remoteEndpoint);
      if (newSocketFd != -1) {
        try {
          et::ConnectResponse response;
          connectHandshake(newSocketFd, &response, /*resetIntent=*/false);
          LOG(INFO) << "Got response with status: " << response.status() << " "
                    << INVALID_KEY;
          if (response.status() == INVALID_KEY) {
            LOG(INFO) << "Got invalid key on reconnect, assume that server has "
                         "terminated the session.";
            // This means that the server has terminated the connection.
            lastStatus_.store(INVALID_KEY);
            shuttingDown = true;
            socketHandler->close(newSocketFd);
            return;
          }
          if (response.status() != RETURNING_CLIENT) {
            STERROR << "Error reconnecting to server: " << response.status()
                    << ": " << response.error();
            CLOG(INFO, "stdout")
                << "Error reconnecting to server: " << response.status() << ": "
                << response.error() << endl;
            socketHandler->close(newSocketFd);
          } else {
            recover(newSocketFd, response.resetrequired(),
                    response.resetsalt());
          }
        } catch (const std::runtime_error& re) {
          LOG(INFO) << "Got failure during reconnect";
          socketHandler->close(newSocketFd);
        } catch (...) {
          LOG(ERROR) << "Got an unknown error!";
          std::exception_ptr eptr = std::current_exception();
          if (eptr) {
            try {
              std::rethrow_exception(eptr);
            } catch (const std::exception& e) {
              LOG(ERROR) << "Uncaught c++ exception: " << e.what();
            }
          } else {
            LOG(ERROR) << "Uncaught c++ exception (unknown)";
          }
        }
      }
    }

    if (isDisconnected()) {
      VLOG_EVERY_N(10, 1) << "Waiting to retry...";
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
  LOG(INFO) << "Reconnect complete";
}
}  // namespace et
