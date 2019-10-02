#include "UserJumphostHandler.hpp"

#include "ETerminal.pb.h"

namespace et {
UserJumphostHandler::UserJumphostHandler(
    shared_ptr<SocketHandler> _jumpClientSocketHandler,
    const string &_idpasskey, const SocketEndpoint &_dstSocketEndpoint,
    shared_ptr<SocketHandler> _routerSocketHandler,
    const SocketEndpoint &routerEndpoint)
    : routerSocketHandler(_routerSocketHandler),
      jumpClientSocketHandler(_jumpClientSocketHandler),
      idpasskey(_idpasskey),
      dstSocketEndpoint(_dstSocketEndpoint) {
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
}

void UserJumphostHandler::run() {
  auto idpasskey_splited = split(idpasskey, '/');
  string id = idpasskey_splited[0];
  string passkey = idpasskey_splited[1];
  TerminalUserInfo tui;
  tui.set_id(id);
  tui.set_passkey(passkey);
  tui.set_uid(getuid());
  tui.set_gid(getgid());

  try {
    routerSocketHandler->writePacket(
        routerFd,
        Packet(TerminalPacketType::TERMINAL_USER_INFO, protoToString(tui)));
  } catch (const std::runtime_error &re) {
    LOG(FATAL) << "Cannot send idpasskey to router: " << re.what();
  }

  InitialPayload payload;
  while (true) {
    Packet initPacket;
    if (!routerSocketHandler->readPacket(routerFd, &initPacket)) {
      continue;
    }
    if (initPacket.getHeader() != TerminalPacketType::JUMPHOST_INIT) {
      LOG(FATAL) << "Invalid jumphost init packet header: "
                 << initPacket.getHeader();
    }
    payload = stringToProto<InitialPayload>(initPacket.getPayload());
    break;
  }
  // Turn off jumphost
  if (!payload.jumphost()) {
    LOG(FATAL) << "Jumphost should be set by the initial client";
  }
  payload.set_jumphost(false);

  jumpclient = shared_ptr<ClientConnection>(new ClientConnection(
      jumpClientSocketHandler, dstSocketEndpoint, id, passkey));

  int connectFailCount = 0;
  while (true) {
    try {
      bool fail = true;
      if (jumpclient->connect()) {
        jumpclient->writePacket(
            Packet(EtPacketType::INITIAL_PAYLOAD, protoToString(payload)));
        fd_set rfd;
        timeval tv;
        for (int a = 0; a < 3; a++) {
          FD_ZERO(&rfd);
          int clientFd = jumpclient->getSocketFd();
          if (clientFd < 0) {
            sleep(1);
            continue;
          }
          FD_SET(clientFd, &rfd);
          tv.tv_sec = 1;
          tv.tv_usec = 0;
          select(clientFd + 1, &rfd, NULL, NULL, &tv);
          if (FD_ISSET(clientFd, &rfd)) {
            Packet initialResponsePacket;
            if (jumpclient->readPacket(&initialResponsePacket)) {
              if (initialResponsePacket.getHeader() !=
                  EtPacketType::INITIAL_RESPONSE) {
                cout << "Error: Missing initial response\n";
                LOG(FATAL) << "Missing initial response!";
              }
              auto initialResponse = stringToProto<InitialResponse>(
                  initialResponsePacket.getPayload());
              if (initialResponse.has_error()) {
                cout << "Error initializing connection: "
                     << initialResponse.error() << endl;
                exit(1);
              }
              fail = false;
              break;
            }
          }
        }
      }
      if (fail) {
        LOG(ERROR) << "Connecting to server failed: Connect timeout";
        connectFailCount++;
        if (connectFailCount == 3) {
          throw std::runtime_error("Connect Timeout");
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

  bool run = true;
  bool is_reconnecting = false;
  time_t keepaliveTime = time(NULL) + SERVER_KEEP_ALIVE_DURATION;

  while (run && !jumpclient->isShuttingDown()) {
    {
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        break;
      }
    }
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_SET(routerFd, &rfd);
    int maxfd = routerFd;
    int jumpClientFd = jumpclient->getSocketFd();
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
          Packet p;
          if (routerSocketHandler->readPacket(routerFd, &p)) {
            jumpclient->writePacket(p);
            VLOG(3) << "Sent message from router to dst terminal: "
                    << p.length() << " Header: " << int(p.getHeader());
          }
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
                    << receivedMessage.length()
                    << " Header: " << int(receivedMessage.getHeader()) << endl;
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