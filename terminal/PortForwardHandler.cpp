#include "PortForwardHandler.hpp"

namespace et {
PortForwardHandler::PortForwardHandler(shared_ptr<SocketHandler> _socketHandler)
    : socketHandler(_socketHandler) {}

void PortForwardHandler::update(vector<PortForwardDestinationRequest>* requests,
                                vector<PortForwardData>* dataToSend) {
  for (auto& it : sourceHandlers) {
    it->update(dataToSend);
    int fd = it->listen();
    if (fd >= 0) {
      PortForwardDestinationRequest pfr;
      pfr.set_port(it->getDestinationPort());
      pfr.set_fd(fd);
      requests->push_back(pfr);
    }
  }

  for (auto& it : destinationHandlers) {
    it.second->update(dataToSend);
    if (it.second->getFd() == -1) {
      // Kill the handler and don't update the rest: we'll pick
      // them up later
      destinationHandlers.erase(it.first);
      break;
    }
  }
}

PortForwardSourceResponse PortForwardHandler::createSource(
    const PortForwardSourceRequest& pfsr) {
  try {
    auto handler = shared_ptr<PortForwardSourceHandler>(
        new PortForwardSourceHandler(
            socketHandler, pfsr.sourceport(),
            pfsr.destinationport()));
    sourceHandlers.push_back(handler);
    return PortForwardSourceResponse();
  } catch (const std::runtime_error& ex) {
    PortForwardSourceResponse pfsr;
    pfsr.set_error(ex.what());
    return pfsr;
  }
}

PortForwardDestinationResponse PortForwardHandler::createDestination(
    const PortForwardDestinationRequest& pfdr) {
  // Try ipv6 first
  int fd = socketHandler->connect("::1", pfdr.port());
  if (fd == -1) {
    // Try ipv4 next
    fd = socketHandler->connect("127.0.0.1", pfdr.port());
  }
  PortForwardDestinationResponse pfdresponse;
  pfdresponse.set_clientfd(pfdr.fd());
  if (fd == -1) {
    pfdresponse.set_error(strerror(errno));
  } else {
    int socketId = rand();
    int attempts = 0;
    while (destinationHandlers.find(socketId) != destinationHandlers.end()) {
      socketId = rand();
      attempts++;
      if (attempts >= 100000) {
        pfdresponse.set_error("Could not find empty socket id");
        break;
      }
    }
    if (!pfdresponse.has_error()) {
      LOG(INFO) << "Created socket/fd pair: " << socketId << ' ' << fd;
      destinationHandlers[socketId] = shared_ptr<PortForwardDestinationHandler>(
          new PortForwardDestinationHandler(socketHandler, fd, socketId));
      pfdresponse.set_socketid(socketId);
    }
  }
  return pfdresponse;
}

void PortForwardHandler::handlePacket(char packetType,
                                      shared_ptr<Connection> connection) {
  switch (packetType) {
    case PacketType::PORT_FORWARD_DATA: {
      PortForwardData pwd = connection->readProto<PortForwardData>();
      if (pwd.sourcetodestination()) {
        LOG(INFO) << "Got data for destination socket: " << pwd.socketid();
        auto it = destinationHandlers.find(pwd.socketid());
        if (it == destinationHandlers.end()) {
          LOG(ERROR) << "Got data for a socket id that has already closed: "
                     << pwd.socketid();
        } else {
          if (pwd.has_closed()) {
            LOG(INFO) << "Port forward socket closed: " << pwd.socketid();
            it->second->close();
            destinationHandlers.erase(it);
          } else if (pwd.has_error()) {
            // TODO: Probably need to do something better here
            LOG(INFO) << "Port forward socket errored: " << pwd.socketid();
            it->second->close();
            destinationHandlers.erase(it);
          } else {
            it->second->write(pwd.buffer());
          }
        }
      } else {
        if (pwd.has_closed()) {
          LOG(INFO) << "Port forward socket closed: " << pwd.socketid();
          closeSourceSocketId(pwd.socketid());
        } else if (pwd.has_error()) {
          LOG(INFO) << "Port forward socket errored: " << pwd.socketid();
          closeSourceSocketId(pwd.socketid());
        } else {
          LOG(INFO) << "Got data for source socket: " << pwd.socketid();
          sendDataToSourceOnSocket(pwd.socketid(), pwd.buffer());
        }
      }
      break;
    }
    case PacketType::PORT_FORWARD_SOURCE_REQUEST: {
      LOG(INFO) << "Got new port source request";
      PortForwardSourceRequest pfsr = connection->readProto<PortForwardSourceRequest>();
      PortForwardSourceResponse pfsresponse = createSource(pfsr);
      char c = PacketType::PORT_FORWARD_SOURCE_RESPONSE;
      connection->writeMessage(string(1, c));
      connection->writeProto(pfsresponse);
      break;
    }
    case PacketType::PORT_FORWARD_SOURCE_RESPONSE: {
      LOG(INFO) << "Got port source response";
      PortForwardSourceResponse pfsresponse = connection->readProto<PortForwardSourceResponse>();
      if (pfsresponse.has_error()) {
        cerr << "FATAL: A reverse tunnel has failed (probably because someone else is already using that port on the destination server" << endl;
        LOG(FATAL) << "Reverse tunnel request failed: " << pfsresponse.error();
      }
      break;
    }
    case PacketType::PORT_FORWARD_DESTINATION_REQUEST: {
      PortForwardDestinationRequest pfdr = connection->readProto<PortForwardDestinationRequest>();
      LOG(INFO) << "Got new port destination request for port " << pfdr.port();
      PortForwardDestinationResponse pfdresponse = createDestination(pfdr);
      char c = PacketType::PORT_FORWARD_DESTINATION_RESPONSE;
      connection->writeMessage(string(1, c));
      connection->writeProto(pfdresponse);
      break;
    }
    case PacketType::PORT_FORWARD_DESTINATION_RESPONSE: {
      PortForwardDestinationResponse pfdr = connection->readProto<PortForwardDestinationResponse>();
      if (pfdr.has_error()) {
        LOG(INFO) << "Could not connect to server through tunnel: "
                  << pfdr.error();
        closeSourceFd(pfdr.clientfd());
      } else {
        LOG(INFO) << "Received socket/fd map from server: " << pfdr.socketid()
                  << " " << pfdr.clientfd();
        addSourceSocketId(pfdr.socketid(), pfdr.clientfd());
      }
      break;
    }
    default: {
      LOG(FATAL) << "Unknown packet type: " << int(packetType) << endl;
    }
  }
}

void PortForwardHandler::closeSourceFd(int fd) {
  for (auto& it : sourceHandlers) {
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
  for (auto& it : sourceHandlers) {
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
}  // namespace et
