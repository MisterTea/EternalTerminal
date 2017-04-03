#include "ClientConnection.hpp"

namespace et {
const int NULL_CLIENT_ID = -1;

ClientConnection::ClientConnection(
    std::shared_ptr<SocketHandler> _socketHandler,  //
    const std::string& _hostname,                   //
    int _port,                                      //
    const string& _key)
    : Connection(_socketHandler, _key),  //
      hostname(_hostname),               //
      port(_port) {}

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
    if (socketFd == -1) {
      throw std::runtime_error("Could not connect to host");
    }
    VLOG(1) << "Sending null id" << endl;
    et::ConnectRequest request;
    request.set_clientid(NULL_CLIENT_ID);
    request.set_version(PROTOCOL_VERSION);
    socketHandler->writeProto(socketFd, request, true);
    VLOG(1) << "Receiving client id" << endl;
    et::ConnectResponse response =
        socketHandler->readProto<et::ConnectResponse>(socketFd, true);
    if (response.has_error()) {
      LOG(ERROR) << "Error connecting to server: " << response.error();
      cerr << "Error connecting to server: " << response.error() << endl;
      exit(1);
    }
    clientId = response.clientid();
    VLOG(1) << "Creating backed reader" << endl;
    reader = std::shared_ptr<BackedReader>(new BackedReader(
        socketHandler, shared_ptr<CryptoHandler>(
                           new CryptoHandler(key, SERVER_CLIENT_NONCE_MSB)),
        socketFd));
    VLOG(1) << "Creating backed writer" << endl;
    writer = std::shared_ptr<BackedWriter>(new BackedWriter(
        socketHandler, shared_ptr<CryptoHandler>(
                           new CryptoHandler(key, CLIENT_SERVER_NONCE_MSB)),
        socketFd));
    VLOG(1) << "Client Connection established" << endl;
  } catch (const runtime_error& err) {
    if (socketFd != -1) {
      socketHandler->close(socketFd);
    }
    throw err;
  }
}

void ClientConnection::closeSocket() {
  LOG(INFO) << "Closing socket";
  if (reconnectThread.get()) {
    LOG(INFO) << "Waiting for reconnect thread to finish";
    reconnectThread->join();
    reconnectThread.reset();
  }
  {
    // Close the socket
    Connection::closeSocket();
  }
  LOG(INFO) << "Socket closed.  Starting new reconnect thread";
  // Spin up a thread to poll for reconnects
  reconnectThread = std::shared_ptr<std::thread>(
      new std::thread(&ClientConnection::pollReconnect, this));
}

ssize_t ClientConnection::read(string* buf) {
  return Connection::read(buf);
}
ssize_t ClientConnection::write(const string& buf) {
  return Connection::write(buf);
}

void ClientConnection::pollReconnect() {
  while (socketFd == -1) {
    {
      lock_guard<std::recursive_mutex> guard(connectionMutex);
      LOG(INFO) << "Trying to reconnect to " << hostname << ":" << port << endl;
      int newSocketFd = socketHandler->connect(hostname, port);
      if (newSocketFd != -1) {
        try {
          et::ConnectRequest request;
          request.set_clientid(clientId);
          socketHandler->writeProto(newSocketFd, request, true);
          recover(newSocketFd);
        } catch (const std::runtime_error& re) {
          socketHandler->close(newSocketFd);
        }
      }
    }

    if (socketFd == -1) {
      VLOG(1) << "Waiting to retry...";
      sleep(1);
    }
  }
}
}
