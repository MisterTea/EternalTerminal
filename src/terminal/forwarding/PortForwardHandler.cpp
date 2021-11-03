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

#ifdef WIN32
#else
PortForwardSourceResponse PortForwardHandler::createSource(
    const PortForwardSourceRequest& pfsr, string* sourceName, uid_t userid,
    gid_t groupid) {
  try {
    if (pfsr.has_source() && sourceName) {
      throw runtime_error(
          "Do not set a source when forwarding named pipes with environment "
          "variables");
    }
    SocketEndpoint source;
    if (pfsr.has_source()) {
      source = pfsr.source();
      if (source.has_name()) {
        throw runtime_error(
            "Named socket tunneling is only allowed with temporary filenames.");
      }
    } else {
      // Make a random file to forward the pipe
      string sourcePattern =
          GetTempDirectory() + string("et_forward_sock_XXXXXX");
      string sourceDirectory = string(mkdtemp(&sourcePattern[0]));
      FATAL_FAIL(::chmod(sourceDirectory.c_str(), S_IRUSR | S_IWUSR | S_IXUSR));
      FATAL_FAIL(::chown(sourceDirectory.c_str(), userid, groupid));
      string sourcePath = string(sourceDirectory) + "/sock";

      source.set_name(sourcePath);
      if (sourceName == nullptr) {
        STFATAL
            << "Tried to create a pipe but without a place to put the name!";
      }
      *sourceName = sourcePath;
      LOG(INFO) << "Creating pipe at " << sourcePath;
    }
    if (pfsr.source().has_port()) {
      if (sourceName != nullptr) {
        STFATAL << "Tried to create a port forward but with a place to put "
                   "the name!";
      }
      auto handler = shared_ptr<ForwardSourceHandler>(new ForwardSourceHandler(
          networkSocketHandler, source, pfsr.destination()));
      sourceHandlers.push_back(handler);
      return PortForwardSourceResponse();
    } else {
      if (userid < 0 || groupid < 0) {
        STFATAL
            << "Tried to create a unix socket forward with no userid/groupid";
      }
      auto handler = shared_ptr<ForwardSourceHandler>(new ForwardSourceHandler(
          pipeSocketHandler, source, pfsr.destination()));
      FATAL_FAIL(::chmod(source.name().c_str(), S_IRUSR | S_IWUSR | S_IXUSR));
      FATAL_FAIL(::chown(source.name().c_str(), userid, groupid));
      sourceHandlers.push_back(handler);
      return PortForwardSourceResponse();
    }
  } catch (const std::runtime_error& ex) {
    PortForwardSourceResponse pfsr;
    pfsr.set_error(ex.what());
    return pfsr;
  }
}
#endif

PortForwardDestinationResponse PortForwardHandler::createDestination(
    const PortForwardDestinationRequest& pfdr) {
  int fd = -1;
  bool isTcp = pfdr.destination().has_port();
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
    pfdresponse.set_error(strerror(GetErrno()));
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
      destinationHandlers[socketId] =
          shared_ptr<ForwardDestinationHandler>(new ForwardDestinationHandler(
              isTcp ? networkSocketHandler : pipeSocketHandler, fd, socketId));
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
          LOG(WARNING) << "Got data for a socket id that has already closed: "
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
      STFATAL << "Unknown packet type: " << int(packet.getHeader());
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
  STERROR << "Tried to close an unassigned socket that didn't exist (maybe "
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
  STERROR << "Tried to add a socketId but the corresponding sourceFd is "
             "already dead: "
          << socketId << " " << sourceFd;
}

void PortForwardHandler::closeSourceSocketId(int socketId) {
  auto it = socketIdSourceHandlerMap.find(socketId);
  if (it == socketIdSourceHandlerMap.end()) {
    STERROR << "Tried to close a socket id that doesn't exist";
    return;
  }
  it->second->closeSocket(socketId);
  socketIdSourceHandlerMap.erase(socketId);
}

void PortForwardHandler::sendDataToSourceOnSocket(int socketId,
                                                  const string& data) {
  auto it = socketIdSourceHandlerMap.find(socketId);
  if (it == socketIdSourceHandlerMap.end()) {
    STERROR << "Tried to send data on a socket id that doesn't exist: "
            << socketId;
    return;
  }
  it->second->sendDataOnSocket(socketId, data);
}
}  // namespace et
