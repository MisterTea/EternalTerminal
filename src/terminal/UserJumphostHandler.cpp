#include "UserJumphostHandler.hpp"

#include "ETerminal.pb.h"

namespace et {
UserJumphostHandler::UserJumphostHandler(
    shared_ptr<SocketHandler> jumpClientSocketHandler, const string &idpasskey,
    const SocketEndpoint &dstSocketEndpoint,
    shared_ptr<SocketHandler> _routerSocketHandler,
    const SocketEndpoint &routerEndpoint)
    : routerSocketHandler(_routerSocketHandler) {
  auto idpasskey_splited = split(idpasskey, '/');
  string id = idpasskey_splited[0];
  string passkey = idpasskey_splited[1];

  routerFd = routerSocketHandler->connect(routerEndpoint);

  if (routerFd < 0) {
    if (errno == ECONNREFUSED) {
      cout << "Error:  The Eternal Terminal daemon is not running.  Please "
              "(re)start the et daemon on the server."
           << endl;
    } else {
      cout << "Error:  Connection error communicating with et daemon: "
           << strerror(errno) << "." << endl;
    }
    exit(1);
  }

  try {
    routerSocketHandler->writePacket(
        routerFd, Packet(TerminalPacketType::IDPASSKEY, idpasskey));
  } catch (const std::runtime_error &re) {
    LOG(FATAL) << "Cannot send idpasskey to router: " << re.what();
  }

  InitialPayload payload;

  jumpclient = shared_ptr<ClientConnection>(new ClientConnection(
      jumpClientSocketHandler, dstSocketEndpoint, id, passkey));

  int connectFailCount = 0;
  while (true) {
    try {
      if (jumpclient->connect()) {
        jumpclient->writePacket(
            Packet(et::EtPacketType::INITIAL_PAYLOAD, protoToString(payload)));
        break;
      } else {
        LOG(ERROR) << "Connecting to dst server failed: Connect timeout";
        connectFailCount++;
        if (connectFailCount == 3) {
          throw std::runtime_error("Connect timeout");
        }
      }
    } catch (const runtime_error &err) {
      LOG(INFO) << "Could not make initial connection to dst server";
      cout << "Could not make initial connection to " << dstSocketEndpoint
           << ": " << err.what() << endl;
      exit(1);
    }
    break;
  }
  VLOG(1) << "JumpClient created with id: " << jumpclient->getId();
}

void UserJumphostHandler::run() {
  bool run = true;
  bool is_reconnecting = false;
  time_t keepaliveTime = time(NULL) + SERVER_KEEP_ALIVE_DURATION;

  while (run && !jumpclient->isShuttingDown()) {
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_SET(routerFd, &rfd);
    int maxfd = routerFd;
    int jumpClientFd = jumpclient->getSocketFd();
    VLOG(4) << "Jump cliend fd: " << jumpClientFd;
    if (jumpClientFd > 0) {
      FD_SET(jumpClientFd, &rfd);
      maxfd = max(maxfd, jumpClientFd);
    }
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, NULL, NULL, &tv);

    try {
      // forward local router -> DST terminal.
      if (FD_ISSET(routerFd, &rfd)) {
        VLOG(4) << "Routerfd is selected";
        if (jumpClientFd < 0) {
          if (is_reconnecting) {
            // there is a reconnect thread running, joining...
            jumpclient->waitReconnect();
            is_reconnecting = false;
          } else {
            LOG(INFO) << "User comes back, reconnecting";
            is_reconnecting = true;
            jumpclient->closeSocketAndMaybeReconnect();
          }
          LOG(INFO) << "Reconnecting, sleep for 3s...";
          sleep(3);
          continue;
        } else {
          Packet p = routerSocketHandler->readPacket(routerFd);
          jumpclient->writePacket(p);
          VLOG(3) << "Sent message from router to dst terminal: " << p.length();
        }
        keepaliveTime = time(NULL) + SERVER_KEEP_ALIVE_DURATION;
      }
      // forward DST terminal -> local router
      if (jumpClientFd > 0 && FD_ISSET(jumpClientFd, &rfd)) {
        if (jumpclient->hasData()) {
          Packet receivedMessage;
          if (jumpclient->readPacket(&receivedMessage)) {
            routerSocketHandler->writePacket(routerFd, receivedMessage);
            VLOG(3) << "Send message from dst terminal to router: "
                    << receivedMessage.length();
          }
        }
        keepaliveTime = time(NULL) + SERVER_KEEP_ALIVE_DURATION;
      }
      // src disconnects, close jump -> dst
      if (jumpClientFd > 0 && keepaliveTime < time(NULL)) {
        LOG(INFO) << "Jumpclient idle, killing connection";
        jumpclient->closeSocket();
        is_reconnecting = false;
      }
    } catch (const runtime_error &re) {
      LOG(ERROR) << "Error: " << re.what();
      cout << "Connection closing because of error: " << re.what() << endl;
      run = false;
    }
  }
  LOG(ERROR) << "Jumpclient shutdown";
  close(routerFd);
}
}  // namespace et