#include "ClientConnection.hpp"

namespace et {
const int NULL_CLIENT_ID = -1;

ClientConnection::ClientConnection ( std::shared_ptr< SocketHandler > _socketHandler,  //
                                     const std::string& _hostname,                     //
                                     int _port,                                        //
                                     const string& _key )
    : Connection ( _socketHandler, _key ),  //
      hostname ( _hostname ),               //
      port ( _port ) {}

ClientConnection::~ClientConnection ( ) {
  if ( reconnectThread ) {
    reconnectThread->join ( );
    reconnectThread.reset ( );
  }
}

void ClientConnection::connect ( ) {
  try {
    VLOG(1) << "Connecting" << endl;
    socketFd = socketHandler->connect(hostname, port);
    VLOG(1) << "Sending null id" << endl;
    et::ConnectRequest request;
    request.set_clientid(NULL_CLIENT_ID);
    socketHandler->writeProto(socketFd, request);
    VLOG(1) << "Receiving client id" << endl;
    et::ConnectResponse response =
      socketHandler->readProto<et::ConnectResponse>(socketFd);
    clientId = response.clientid();
    VLOG(1) << "Creating backed reader" << endl;
    reader = std::shared_ptr<BackedReader>(
      new BackedReader(
        socketHandler,
        shared_ptr<CryptoHandler>(new CryptoHandler(key)),
        socketFd));
    VLOG(1) << "Creating backed writer" << endl;
    writer = std::shared_ptr<BackedWriter>(
      new BackedWriter(
        socketHandler,
        shared_ptr<CryptoHandler>(new CryptoHandler(key)),
        socketFd));
    VLOG(1) << "Client Connection established" << endl;
  } catch (const runtime_error& err) {
    socketHandler->close(socketFd);
    throw err;
  }
}

void ClientConnection::closeSocket ( ) {
  Connection::closeSocket ( );

  if ( reconnectThread.get ( ) ) {
    reconnectThread->join ( );
    reconnectThread.reset ( );
  }
  if ( socketFd == -1 ) {
    // Spin up a thread to poll for reconnects
    reconnectThread = std::shared_ptr< std::thread > ( new std::thread ( &ClientConnection::pollReconnect, this ) );
  }
}

void ClientConnection::pollReconnect() {
  while (socketFd == -1) {
    LOG(INFO) << "Trying to reconnect to " << hostname << ":" << port << endl;
    int newSocketFd = socketHandler->connect(hostname, port);
    if (newSocketFd != -1) {
      et::ConnectRequest request;
      request.set_clientid(clientId);
      socketHandler->writeProto(newSocketFd, request);

      recover ( newSocketFd );
    } else {
      VLOG ( 1 ) << "Waiting to retry...";
      sleep ( 1 );
    }
  }
}
}
