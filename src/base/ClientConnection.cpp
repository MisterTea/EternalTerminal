#include "ClientConnection.hpp"

namespace et {
ClientConnection::ClientConnection(
    std::shared_ptr<SocketHandler> _socketHandler,
    const SocketEndpoint& _remoteEndpoint, const string& _id,
    const string& _key)
    : Connection(_socketHandler, _id, _key), remoteEndpoint(_remoteEndpoint) {}

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
    et::ConnectRequest request;
    request.set_clientid(id);
    request.set_version(PROTOCOL_VERSION);
    socketHandler->writeProto(socketFd, request, true);
    VLOG(1) << "Receiving client id";
    et::ConnectResponse response =
        socketHandler->readProto<et::ConnectResponse>(socketFd, true);
    if (response.status() != NEW_CLIENT &&
        response.status() != RETURNING_CLIENT) {
      // Note: the response can be returning client if the client died while
      // performing the initial connection but the server thought the client
      // survived.
      STERROR << "Error connecting to server: " << response.status() << ": "
              << response.error();
      cout << "Error connecting to server: " << response.status() << ": "
           << response.error() << endl;
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
    VLOG(1) << "Client Connection established";
    return true;
  } catch (const runtime_error& err) {
    LOG(INFO) << "Got failure during connect";
    if (socketFd != -1) {
      socketHandler->close(socketFd);
    }
  }
  return false;
}

void ClientConnection::closeSocketAndMaybeReconnect() {
  waitReconnect();
  LOG(INFO) << "Closing socket";
  closeSocket();
  LOG(INFO) << "Socket closed, starting new reconnect thread";
  reconnectThread = std::shared_ptr<std::thread>(
      new std::thread(&ClientConnection::pollReconnect, this));
}

void ClientConnection::waitReconnect() {
  if (reconnectThread.get()) {
    LOG(INFO) << "Waiting for reconnect thread to finish";
    reconnectThread->join();
    reconnectThread.reset();
  }
}

void ClientConnection::pollReconnect() {
  el::Helpers::setThreadName("Reconnect");
  LOG(INFO) << "Trying to reconnect to " << remoteEndpoint << endl;
  while (socketFd == -1) {
    {
      lock_guard<std::recursive_mutex> guard(connectionMutex);
      LOG_EVERY_N(10, INFO) << "In reconnect loop " << remoteEndpoint << endl;
      int newSocketFd = socketHandler->connect(remoteEndpoint);
      if (newSocketFd != -1) {
        try {
          et::ConnectRequest request;
          request.set_clientid(id);
          request.set_version(PROTOCOL_VERSION);
          socketHandler->writeProto(newSocketFd, request, true);
          et::ConnectResponse response =
              socketHandler->readProto<et::ConnectResponse>(newSocketFd, true);
          LOG(INFO) << "Got response with status: " << response.status() << " "
                    << INVALID_KEY;
          if (response.status() == INVALID_KEY) {
            LOG(INFO) << "Got invalid key on reconnect, assume that server has "
                         "terminated the session.";
            // This means that the server has terminated the connection.
            shuttingDown = true;
            socketHandler->close(newSocketFd);
            return;
          }
          if (response.status() != RETURNING_CLIENT) {
            STERROR << "Error reconnecting to server: " << response.status()
                    << ": " << response.error();
            cout << "Error reconnecting to server: " << response.status()
                 << ": " << response.error() << endl;
            socketHandler->close(newSocketFd);
          } else {
            recover(newSocketFd);
          }
        } catch (const std::runtime_error& re) {
          LOG(INFO) << "Got failure during reconnect";
          socketHandler->close(newSocketFd);
        }
      }
    }

    if (socketFd == -1) {
      VLOG_EVERY_N(10, 1) << "Waiting to retry...";
      usleep(1000 * 1000);
    }
  }
  LOG(INFO) << "Reconnect complete";
}
}  // namespace et
