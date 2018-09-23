#include "PortForwardSourceHandler.hpp"

namespace et {
PortForwardSourceHandler::PortForwardSourceHandler(
    shared_ptr<SocketHandler> _socketHandler, int _sourcePort,
    int _destinationPort)
    : socketHandler(_socketHandler),
      sourcePort(_sourcePort),
      destinationPort(_destinationPort) {
  socketHandler->listen(SocketEndpoint(sourcePort));
}

int PortForwardSourceHandler::listen() {
  // TODO: Replace with select
  for (int i : socketHandler->getEndpointFds(SocketEndpoint(sourcePort))) {
    int fd = socketHandler->accept(i);
    if (fd > -1) {
      LOG(INFO) << "Tunnel " << sourcePort << " -> " << destinationPort
                << " socket created with fd " << fd;
      unassignedFds.insert(fd);
      return fd;
    }
  }
  return -1;
}

void PortForwardSourceHandler::update(vector<PortForwardData>* data) {
  vector<int> socketsToRemove;

  for (auto& it : socketFdMap) {
    int socketId = it.first;
    int fd = it.second;

    while (socketHandler->hasData(fd)) {
      char buf[1024];
      int bytesRead = socketHandler->read(fd, buf, 1024);
      if (bytesRead == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // Bail for now
        break;
      }
      PortForwardData pwd;
      pwd.set_socketid(socketId);
      pwd.set_sourcetodestination(true);
      if (bytesRead == -1) {
        VLOG(1) << "Got error reading socket " << socketId << " "
                << strerror(errno);
        pwd.set_error(strerror(errno));
      } else if (bytesRead == 0) {
        VLOG(1) << "Got close reading socket " << socketId;
        pwd.set_closed(true);
      } else {
        VLOG(1) << "Reading " << bytesRead << " bytes from socket " << socketId;
        pwd.set_buffer(string(buf, bytesRead));
      }
      data->push_back(pwd);
      if (bytesRead < 1) {
        socketHandler->close(fd);
        socketsToRemove.push_back(socketId);
        break;
      }
    }
  }
  for (auto& it : socketsToRemove) {
    socketFdMap.erase(it);
  }
}

bool PortForwardSourceHandler::hasUnassignedFd(int fd) {
  return unassignedFds.find(fd) != unassignedFds.end();
}

void PortForwardSourceHandler::closeUnassignedFd(int fd) {
  if (unassignedFds.find(fd) == unassignedFds.end()) {
    LOG(ERROR) << "Tried to close an unassigned fd that doesn't exist";
    return;
  }
  socketHandler->close(fd);
  unassignedFds.erase(fd);
}

void PortForwardSourceHandler::addSocket(int socketId, int sourceFd) {
  if (unassignedFds.find(sourceFd) == unassignedFds.end()) {
    LOG(ERROR) << "Tried to close an unassigned fd that doesn't exist "
               << sourceFd;
    return;
  }
  LOG(INFO) << "Adding socket: " << socketId << " " << sourceFd;
  unassignedFds.erase(sourceFd);
  socketFdMap[socketId] = sourceFd;
}

void PortForwardSourceHandler::sendDataOnSocket(int socketId,
                                                const string& data) {
  if (socketFdMap.find(socketId) == socketFdMap.end()) {
    LOG(ERROR) << "Tried to write to a socket that no longer exists!";
    return;
  }

  int fd = socketFdMap[socketId];
  const char* buf = data.c_str();
  int count = data.length();
  socketHandler->writeAllOrReturn(fd, buf, count);
}

void PortForwardSourceHandler::closeSocket(int socketId) {
  auto it = socketFdMap.find(socketId);
  if (it == socketFdMap.end()) {
    LOG(ERROR) << "Tried to remove a socket that no longer exists!";
  } else {
    socketHandler->close(it->second);
    socketFdMap.erase(it);
  }
}
}  // namespace et
