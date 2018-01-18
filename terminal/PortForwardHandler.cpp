#include "PortForwardHandler.hpp"

namespace et {
PortForwardHandler::PortForwardHandler(shared_ptr<SocketHandler> _socketHandler) :
    socketHandler(_socketHandler) {
}

void PortForwardHandler::update(
    vector<PortForwardRequest>* requests,
    vector<PortForwardData>* dataToSend) {
  for (auto& it : handlers) {
    it->update(dataToSend);
    int fd = it->listen();
    if (fd >= 0) {
      PortForwardRequest pfr;
      pfr.set_port(it->getDestinationPort());
      pfr.set_fd(fd);
      requests->push_back(pfr);
    }
  }

  for (auto &it : portForwardHandlers) {
    it.second->update(dataToSend);
    if (it.second->getFd() == -1) {
      // Kill the handler and don't update the rest: we'll pick
      // them up later
      portForwardHandlers.erase(it.first);
      break;
    }
  }
}

PortForwardResponse PortForwardHandler::createDestination(const PortForwardRequest& pfr) {
  // Try ipv6 first
  int fd = socketHandler->connect("::1", pfr.port());
  if (fd == -1) {
    // Try ipv4 next
    fd = socketHandler->connect("127.0.0.1", pfr.port());
  }
  PortForwardResponse pfresponse;
  pfresponse.set_clientfd(pfr.fd());
  if (fd == -1) {
    pfresponse.set_error(strerror(errno));
  } else {
    int socketId = rand();
    int attempts = 0;
    while (portForwardHandlers.find(socketId) !=
           portForwardHandlers.end()) {
      socketId = rand();
      attempts++;
      if (attempts >= 100000) {
        pfresponse.set_error("Could not find empty socket id");
        break;
      }
    }
    if (!pfresponse.has_error()) {
      LOG(INFO)
          << "Created socket/fd pair: " << socketId << ' ' << fd;
      portForwardHandlers[socketId] =
          shared_ptr<PortForwardDestinationHandler>(
              new PortForwardDestinationHandler(socketHandler, fd,
                                                socketId));
      pfresponse.set_socketid(socketId);
    }
  }
  return pfresponse;
}

void PortForwardHandler::handlePacket(
    char packetType,
    shared_ptr<ServerClientConnection> serverClientState) {
  switch (packetType) {
    case PacketType::PORT_FORWARD_DESTINATION_REQUEST: {
      LOG(INFO) << "Got new port forward";
      PortForwardRequest pfr =
          serverClientState->readProto<PortForwardRequest>();
      PortForwardResponse pfresponse = createDestination(pfr);
      char c = PacketType::PORT_FORWARD_DESTINATION_RESPONSE;
      serverClientState->writeMessage(string(1, c));
      serverClientState->writeProto(pfresponse);
      break;
    }
    case PacketType::PORT_FORWARD_SD_DATA: {
      PortForwardData pwd =
          serverClientState->readProto<PortForwardData>();
      LOG(INFO) << "Got data for socket: " << pwd.socketid();
      auto it = portForwardHandlers.find(pwd.socketid());
      if (it == portForwardHandlers.end()) {
        LOG(ERROR)
            << "Got data for a socket id that has already closed: "
            << pwd.socketid();
      } else {
        if (pwd.has_closed()) {
          LOG(INFO) << "Port forward socket closed: " << pwd.socketid();
          it->second->close();
          portForwardHandlers.erase(it);
        } else if (pwd.has_error()) {
          // TODO: Probably need to do something better here
          LOG(INFO)
              << "Port forward socket errored: " << pwd.socketid();
          it->second->close();
          portForwardHandlers.erase(it);
        } else {
          it->second->write(pwd.buffer());
        }
      }
      break;
    }
    default: {
      LOG(FATAL) << "Unknown packet type: " << int(packetType) << endl;
    }
  }
}

void PortForwardHandler::addSourceHandler(
    shared_ptr<PortForwardSourceHandler> handler) {
  handlers.push_back(handler);
}

void PortForwardHandler::closeSourceFd(int fd) {
  for (auto& it : handlers) {
    if (it->hasUnassignedFd(fd)) {
      it->closeUnassignedFd(fd);
      return;
    }
  }
  LOG(ERROR) << "Tried to close an unassigned socket that didn't exist (maybe "
                "it was already removed?): "
             << fd;
}

void PortForwardHandler::addSourceSocketId(int socketId, int sourceFd) {
  for (auto& it : handlers) {
    if (it->hasUnassignedFd(sourceFd)) {
      it->addSocket(socketId, sourceFd);
      socketIdSourceHandlerMap[socketId] = it;
      return;
    }
  }
  LOG(ERROR) << "Tried to add a socketId but the corresponding sourceFd is "
                "already dead: "
             << socketId << " " << sourceFd;
}

void PortForwardHandler::closeSourceSocketId(int socketId) {
  auto it = socketIdSourceHandlerMap.find(socketId);
  if (it == socketIdSourceHandlerMap.end()) {
    LOG(ERROR) << "Tried to close a socket id that doesn't exist";
    return;
  }
  it->second->closeSocket(socketId);
  socketIdSourceHandlerMap.erase(socketId);
}

void PortForwardHandler::sendDataToSourceOnSocket(int socketId,
                                               const string& data) {
  auto it = socketIdSourceHandlerMap.find(socketId);
  if (it == socketIdSourceHandlerMap.end()) {
    LOG(ERROR) << "Tried to send data on a socket id that doesn't exist: "
               << socketId;
    return;
  }
  it->second->sendDataOnSocket(socketId, data);
}
}
