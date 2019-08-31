#include "PortForwardHandler.hpp"

namespace et {
PortForwardHandler::PortForwardHandler(
    shared_ptr<SocketHandler> _networkSocketHandler,
    shared_ptr<SocketHandler> _pipeSocketHandler)
    : networkSocketHandler(_networkSocketHandler),
      pipeSocketHandler(_pipeSocketHandler) {}

void PortForwardHandler::update(vector<PortForwardDestinationRequest>* requests,
                                vector<PortForwardData>* dataToSend) {
  for (auto& it : sourceHandlers) {
    it->update(dataToSend);
    int fd = it->listen();
    if (fd >= 0) {
      PortForwardDestinationRequest pfr;
      *(pfr.mutable_destination()) = it->getDestination();
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
    if (pfsr.has_destination() && !pfsr.destination().has_port()) {
      throw runtime_error(
          "Do not set a destination when forwarding named pipes");
    }
    SocketEndpoint destination;
    if (pfsr.has_destination()) {
      destination = pfsr.destination();
    } else {
      // Make a random file to forward the pipe
      string destinationPattern = string("/tmp/et_forward_sock_XXXXXXXX");
      string destinationDirectory = string(mkdtemp(&destinationPattern[0]));
      string destinationPath = string(destinationDirectory) + "/sock";

      destination.set_name(destinationPath);
      LOG(INFO) << "Creating pipe at " << destinationPath;
    }
    if (pfsr.destination().has_port()) {
      auto handler = shared_ptr<ForwardSourceHandler>(new ForwardSourceHandler(
          networkSocketHandler, pfsr.source(), destination));
      sourceHandlers.push_back(handler);
      return PortForwardSourceResponse();
    } else {
      auto handler = shared_ptr<ForwardSourceHandler>(new ForwardSourceHandler(
          pipeSocketHandler, pfsr.source(), destination));
      sourceHandlers.push_back(handler);
      if (pfsr.has_environmentvariable()) {
        if (setenv(pfsr.environmentvariable().c_str(),
                   pfsr.destination().name().c_str(), 1) == -1) {
          throw runtime_error(strerror(errno));
        }
      }
      return PortForwardSourceResponse();
    }
  } catch (const std::runtime_error& ex) {
    PortForwardSourceResponse pfsr;
    pfsr.set_error(ex.what());
    return pfsr;
  }
}

PortForwardDestinationResponse PortForwardHandler::createDestination(
    const PortForwardDestinationRequest& pfdr) {
  int fd = -1;
  if (pfdr.destination().has_port()) {
    // Try ipv6 first
    SocketEndpoint ipv6Localhost;
    ipv6Localhost.set_name("::1");
    ipv6Localhost.set_port(pfdr.destination().port());

    fd = networkSocketHandler->connect(ipv6Localhost);
    if (fd == -1) {
      SocketEndpoint ipv4Localhost;
      ipv4Localhost.set_name("127.0.0.1");
      ipv4Localhost.set_port(pfdr.destination().port());
      // Try ipv4 next
      fd = networkSocketHandler->connect(ipv4Localhost);
    }
  } else {
    fd = pipeSocketHandler->connect(pfdr.destination());
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
      destinationHandlers[socketId] = shared_ptr<ForwardDestinationHandler>(
          new ForwardDestinationHandler(networkSocketHandler, fd, socketId));
      pfdresponse.set_socketid(socketId);
    }
  }
  return pfdresponse;
}

void PortForwardHandler::handlePacket(const Packet& packet,
                                      shared_ptr<Connection> connection) {
  switch (TerminalPacketType(packet.getHeader())) {
    case TerminalPacketType::PORT_FORWARD_DATA: {
      PortForwardData pwd = stringToProto<PortForwardData>(packet.getPayload());
      if (pwd.sourcetodestination()) {
        VLOG(1) << "Got data for destination socket: " << pwd.socketid();
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
          VLOG(1) << "Got data for source socket: " << pwd.socketid();
          sendDataToSourceOnSocket(pwd.socketid(), pwd.buffer());
        }
      }
      break;
    }
    case TerminalPacketType::PORT_FORWARD_SOURCE_REQUEST: {
      LOG(INFO) << "Got new port source request";
      PortForwardSourceRequest pfsr =
          stringToProto<PortForwardSourceRequest>(packet.getPayload());
      PortForwardSourceResponse pfsresponse = createSource(pfsr);
      Packet sendPacket(
          uint8_t(TerminalPacketType::PORT_FORWARD_SOURCE_RESPONSE),
          protoToString(pfsresponse));
      connection->writePacket(sendPacket);
      break;
    }
    case TerminalPacketType::PORT_FORWARD_SOURCE_RESPONSE: {
      LOG(INFO) << "Got port source response";
      PortForwardSourceResponse pfsresponse =
          stringToProto<PortForwardSourceResponse>(packet.getPayload());
      if (pfsresponse.has_error()) {
        cout << "FATAL: A reverse tunnel has failed (probably because someone "
                "else is already using that port on the destination server"
             << endl;
        LOG(FATAL) << "Reverse tunnel request failed: " << pfsresponse.error();
      }
      break;
    }
    case TerminalPacketType::PORT_FORWARD_DESTINATION_REQUEST: {
      PortForwardDestinationRequest pfdr =
          stringToProto<PortForwardDestinationRequest>(packet.getPayload());
      LOG(INFO) << "Got new port destination request for "
                << pfdr.destination();
      PortForwardDestinationResponse pfdresponse = createDestination(pfdr);
      Packet sendPacket(
          uint8_t(TerminalPacketType::PORT_FORWARD_DESTINATION_RESPONSE),
          protoToString(pfdresponse));
      connection->writePacket(sendPacket);
      break;
    }
    case TerminalPacketType::PORT_FORWARD_DESTINATION_RESPONSE: {
      PortForwardDestinationResponse pfdr =
          stringToProto<PortForwardDestinationResponse>(packet.getPayload());
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
      LOG(FATAL) << "Unknown packet type: " << int(packet.getHeader());
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
