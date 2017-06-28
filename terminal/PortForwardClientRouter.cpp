#include "PortForwardClientRouter.hpp"

namespace et {
void PortForwardClientRouter::addListener(
    shared_ptr<PortForwardClientListener> listener) {
  listeners.push_back(listener);
}

void PortForwardClientRouter::update(
    vector<PortForwardRequest>* requests,
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

void PortForwardClientRouter::closeClientFd(int fd) {
  for (auto& it : listeners) {
    if(it->hasUnassignedFd(fd)) {
      it->closeUnassignedFd(fd);
      return;
    }
  }
  LOG(ERROR) << "Tried to close an unassigned socket that didn't exist (maybe it was already removed?): " << fd;
}

void PortForwardClientRouter::addSocketId(int socketId, int clientFd) {
  for (auto& it : listeners) {
    if (it->hasUnassignedFd(clientFd)) {
      it->addSocket(socketId, clientFd);
      socketIdListenerMap[socketId] = it;
      return;
    }
  }
  LOG(ERROR) << "Tried to add a socketId but the corresponding clientFd is already dead: " << socketId << " " << clientFd;
}

void PortForwardClientRouter::closeSocketId(int socketId) {
  auto it = socketIdListenerMap.find(socketId);
  if (it == socketIdListenerMap.end()) {
    LOG(ERROR) << "Tried to close a socket id that doesn't exist";
    return;
  }
  it->second->closeSocket(socketId);
  socketIdListenerMap.erase(socketId);
}

void PortForwardClientRouter::sendDataOnSocket(
    int socketId,
    const string& data) {
  auto it = socketIdListenerMap.find(socketId);
  if (it == socketIdListenerMap.end()) {
    LOG(ERROR) << "Tried to send data on a socket id that doesn't exist: " << socketId;
    return;
  }
  it->second->sendDataOnSocket(socketId, data);
}
}
