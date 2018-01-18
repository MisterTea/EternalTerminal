#include "PortForwardSourceRouter.hpp"

namespace et {
void PortForwardSourceRouter::addListener(
    shared_ptr<PortForwardSourceListener> listener) {
  listeners.push_back(listener);
}

void PortForwardSourceRouter::update(vector<PortForwardRequest>* requests,
                                     vector<PortForwardData>* dataToSend) {
  for (auto& it : listeners) {
    it->update(dataToSend);
    int fd = it->listen();
    if (fd >= 0) {
      PortForwardRequest pfr;
      pfr.set_port(it->getDestinationPort());
      pfr.set_fd(fd);
      requests->push_back(pfr);
    }
  }
}

void PortForwardSourceRouter::closeSourceFd(int fd) {
  for (auto& it : listeners) {
    if (it->hasUnassignedFd(fd)) {
      it->closeUnassignedFd(fd);
      return;
    }
  }
  LOG(ERROR) << "Tried to close an unassigned socket that didn't exist (maybe "
                "it was already removed?): "
             << fd;
}

void PortForwardSourceRouter::addSocketId(int socketId, int sourceFd) {
  for (auto& it : listeners) {
    if (it->hasUnassignedFd(sourceFd)) {
      it->addSocket(socketId, sourceFd);
      socketIdListenerMap[socketId] = it;
      return;
    }
  }
  LOG(ERROR) << "Tried to add a socketId but the corresponding sourceFd is "
                "already dead: "
             << socketId << " " << sourceFd;
}

void PortForwardSourceRouter::closeSocketId(int socketId) {
  auto it = socketIdListenerMap.find(socketId);
  if (it == socketIdListenerMap.end()) {
    LOG(ERROR) << "Tried to close a socket id that doesn't exist";
    return;
  }
  it->second->closeSocket(socketId);
  socketIdListenerMap.erase(socketId);
}

void PortForwardSourceRouter::sendDataOnSocket(int socketId,
                                               const string& data) {
  auto it = socketIdListenerMap.find(socketId);
  if (it == socketIdListenerMap.end()) {
    LOG(ERROR) << "Tried to send data on a socket id that doesn't exist: "
               << socketId;
    return;
  }
  it->second->sendDataOnSocket(socketId, data);
}
}  // namespace et
