#include "ForwardSourceHandler.hpp"

namespace et {
ForwardSourceHandler::ForwardSourceHandler(
    shared_ptr<SocketHandler> _socketHandler, const SocketEndpoint& _source,
    const SocketEndpoint& _destination, bool alreadyListening,
    bool socksDynamic)
    : socketHandler(_socketHandler),
      source(_source),
      destination(_destination),
      socksDynamic(socksDynamic) {
  if (!alreadyListening) {
    socketHandler->listen(source);
  }
}

ForwardSourceHandler::ForwardSourceHandler(
    shared_ptr<SocketHandler> _socketHandler,
    const SocketEndpoint& _destination, int readFd, int writeFd, bool closeFds)
    : socketHandler(_socketHandler),
      destination(_destination),
      stdioMode(true),
      closeOwnedFds(closeFds),
      stdioRequestPending(true),
      stdioReadFd(readFd),
      stdioWriteFd(writeFd) {}

ForwardSourceHandler::~ForwardSourceHandler() {
  for (const auto& socket : socketFdMap) {
    int readFd = socket.second;
    int writeFd = readFd;
    auto writeIt = socketWriteFdMap.find(socket.first);
    if (writeIt != socketWriteFdMap.end()) {
      writeFd = writeIt->second;
    }
    if (closeOwnedFds) {
      socketHandler->close(readFd);
      if (writeFd != readFd && writeFd >= 0) {
        socketHandler->close(writeFd);
      }
    }
  }
  for (int fd : unassignedFds) {
    if (closeOwnedFds) {
      socketHandler->close(fd);
    }
  }
  for (const auto& pending : socksPending) {
    socketHandler->close(pending.first);
  }
  if (!stdioMode) {
    socketHandler->stopListening(source);
  }
}

int ForwardSourceHandler::acceptFixed(const set<int>* readyFds) {
  for (int i : socketHandler->getEndpointFds(source)) {
    if (readyFds != nullptr && readyFds->count(i) == 0) {
      continue;
    }
    int fd = socketHandler->accept(i);
    if (fd > -1) {
      LOG(INFO) << "Tunnel " << source << " -> " << destination
                << " socket created with fd " << fd;
      if (socksDynamic) {
        socksPending[fd] = SocksHandshake();
      } else {
        unassignedFds.insert(fd);
        return fd;
      }
    }
  }
  return -1;
}

void ForwardSourceHandler::advanceSocksHandshakes(const set<int>* readyFds) {
  vector<int> toClose;
  for (auto& it : socksPending) {
    int fd = it.first;
    bool mayHaveData = readyFds == nullptr || readyFds->count(fd) != 0 ||
                       !it.second.input.empty() || socketHandler->hasData(fd);
    if (!mayHaveData) {
      continue;
    }
    while (socketHandler->hasData(fd) || !it.second.input.empty()) {
      if (!it.second.input.empty() && !socketHandler->hasData(fd)) {
        auto status = feedSocksHandshake(&it.second);
        if (!it.second.reply.empty()) {
          socketHandler->writeAllOrReturn(fd, it.second.reply.data(),
                                          it.second.reply.size());
          it.second.reply.clear();
        }
        if (status == SocksParseStatus::Error) {
          LOG(WARNING) << "SOCKS handshake failed on fd " << fd << ": "
                       << it.second.error;
          toClose.push_back(fd);
        }
        break;
      }
      char buf[512];
      int bytesRead = socketHandler->read(fd, buf, sizeof(buf));
      auto readErrno = GetErrno();
      if (bytesRead == -1 &&
          (readErrno == EAGAIN || readErrno == EWOULDBLOCK)) {
        break;
      }
      if (bytesRead <= 0) {
        LOG(INFO) << "SOCKS client closed during handshake on fd " << fd;
        toClose.push_back(fd);
        break;
      }
      it.second.input.append(buf, bytesRead);
      auto status = feedSocksHandshake(&it.second);
      if (!it.second.reply.empty()) {
        socketHandler->writeAllOrReturn(fd, it.second.reply.data(),
                                        it.second.reply.size());
        it.second.reply.clear();
      }
      if (status == SocksParseStatus::Error) {
        LOG(WARNING) << "SOCKS handshake failed on fd " << fd << ": "
                     << it.second.error;
        toClose.push_back(fd);
        break;
      }
      if (status == SocksParseStatus::Complete) {
        break;
      }
    }
  }
  for (int fd : toClose) {
    socketHandler->close(fd);
    socksPending.erase(fd);
  }
}

int ForwardSourceHandler::takeCompletedSocks(SocketEndpoint* destinationOut,
                                             const set<int>* readyFds) {
  advanceSocksHandshakes(readyFds);
  for (auto it = socksPending.begin(); it != socksPending.end(); ++it) {
    if (it->second.complete) {
      int fd = it->first;
      if (destinationOut) {
        *destinationOut = it->second.destination;
      }
      LOG(INFO) << "SOCKS tunnel " << source << " -> " << it->second.destination
                << " ready on fd " << fd;
      unassignedFds.insert(fd);
      socksPending.erase(it);
      return fd;
    }
  }
  return -1;
}

int ForwardSourceHandler::listen(SocketEndpoint* destinationOut,
                                 const set<int>* readyFds) {
  if (stdioMode) {
    if (!stdioRequestPending) {
      return -1;
    }
    stdioRequestPending = false;
    unassignedFds.insert(stdioReadFd);
    if (destinationOut) {
      *destinationOut = destination;
    }
    LOG(INFO) << "Stdio forward ready -> " << destination << " readFd "
              << stdioReadFd << " writeFd " << stdioWriteFd;
    return stdioReadFd;
  }

  if (socksDynamic) {
    int ready = takeCompletedSocks(destinationOut, readyFds);
    if (ready >= 0) {
      return ready;
    }
    // Accept new clients into the SOCKS pending map.
    acceptFixed(readyFds);
    return takeCompletedSocks(destinationOut, readyFds);
  }

  int fd = acceptFixed(readyFds);
  if (fd >= 0 && destinationOut) {
    *destinationOut = destination;
  }
  return fd;
}

bool ForwardSourceHandler::update(vector<PortForwardData>* data,
                                  const set<int>* readyFds) {
  if (socksDynamic) {
    advanceSocksHandshakes(readyFds);
  }

  vector<int> socketsToRemove;

  for (auto& it : socketFdMap) {
    int socketId = it.first;
    int fd = it.second;
    if (readyFds != nullptr && readyFds->count(fd) == 0) {
      continue;
    }

    while (true) {
      if (!stdioMode && !socketHandler->hasData(fd)) {
        break;
      }

      char buf[1024];
      int bytesRead = -1;
      int readErrno = 0;
      if (stdioMode) {
#ifndef WIN32
        bytesRead = ::read(fd, buf, 1024);
        readErrno = errno;
        SetErrno(readErrno);
#else
        bytesRead = socketHandler->read(fd, buf, 1024);
        readErrno = GetErrno();
#endif
      } else {
        bytesRead = socketHandler->read(fd, buf, 1024);
        readErrno = GetErrno();
      }
      if (bytesRead == -1 &&
          (readErrno == EAGAIN || readErrno == EWOULDBLOCK)) {
        break;
      }
      PortForwardData pwd;
      pwd.set_socketid(socketId);
      pwd.set_sourcetodestination(true);
      if (bytesRead == -1) {
        VLOG(1) << "Got error reading socket " << socketId << " "
                << strerror(readErrno);
        pwd.set_error(strerror(readErrno));
      } else if (bytesRead == 0) {
        VLOG(1) << "Got close reading socket " << socketId;
        pwd.set_closed(true);
      } else {
        VLOG(1) << "Reading " << bytesRead << " bytes from socket " << socketId;
        pwd.set_buffer(string(buf, bytesRead));
      }
      data->push_back(pwd);
      if (bytesRead < 1) {
        if (closeOwnedFds) {
          socketHandler->close(fd);
          auto writeIt = socketWriteFdMap.find(socketId);
          if (writeIt != socketWriteFdMap.end() && writeIt->second != fd) {
            socketHandler->close(writeIt->second);
          }
        }
        socketsToRemove.push_back(socketId);
        break;
      }
      if (stdioMode) {
        // Outer poll drives stdio readiness; one chunk per wake is enough.
        break;
      }
    }
  }
  for (auto& it : socketsToRemove) {
    socketFdMap.erase(it);
    socketWriteFdMap.erase(it);
  }
  return !socketsToRemove.empty();
}

bool ForwardSourceHandler::hasUnassignedFd(int fd) {
  return unassignedFds.find(fd) != unassignedFds.end();
}

void ForwardSourceHandler::closeUnassignedFd(int fd) {
  if (unassignedFds.find(fd) == unassignedFds.end()) {
    STERROR << "Tried to close an unassigned fd that doesn't exist";
    return;
  }
  if (closeOwnedFds) {
    socketHandler->close(fd);
  }
  unassignedFds.erase(fd);
}

void ForwardSourceHandler::addSocket(int socketId, int sourceFd) {
  if (unassignedFds.find(sourceFd) == unassignedFds.end()) {
    STERROR << "Tried to close an unassigned fd that doesn't exist "
            << sourceFd;
    return;
  }
  LOG(INFO) << "Adding socket: " << socketId << " " << sourceFd;
  unassignedFds.erase(sourceFd);
  socketFdMap[socketId] = sourceFd;
  if (stdioMode) {
    socketWriteFdMap[socketId] = stdioWriteFd;
  }
}

void ForwardSourceHandler::getActiveFds(set<int>* fds) {
  if (stdioMode) {
    if (stdioRequestPending || unassignedFds.count(stdioReadFd) ||
        !socketFdMap.empty()) {
      fds->insert(stdioReadFd);
    }
    return;
  }
  for (int fd : socketHandler->getEndpointFds(source)) {
    fds->insert(fd);
  }
  for (const auto& pending : socksPending) {
    fds->insert(pending.first);
  }
  for (auto& it : socketFdMap) {
    fds->insert(it.second);
  }
}

void ForwardSourceHandler::sendDataOnSocket(int socketId, const string& data) {
  if (socketFdMap.find(socketId) == socketFdMap.end()) {
    LOG(INFO) << "Tried to write to a socket that no longer exists!";
    return;
  }

  int fd = socketFdMap[socketId];
  auto writeIt = socketWriteFdMap.find(socketId);
  if (writeIt != socketWriteFdMap.end()) {
    fd = writeIt->second;
  }
  const char* buf = data.c_str();
  int count = data.length();
  if (stdioMode) {
#ifdef WIN32
    socketHandler->writeAllOrReturn(fd, buf, count);
#else
    size_t written = 0;
    while (written < static_cast<size_t>(count)) {
      ssize_t w = ::write(fd, buf + written, count - written);
      if (w < 0) {
        if (errno == EINTR) {
          continue;
        }
        LOG(WARNING) << "Stdio forward write failed: " << strerror(errno);
        return;
      }
      written += static_cast<size_t>(w);
    }
#endif
  } else {
    socketHandler->writeAllOrReturn(fd, buf, count);
  }
}

void ForwardSourceHandler::closeSocket(int socketId) {
  auto it = socketFdMap.find(socketId);
  if (it == socketFdMap.end()) {
    LOG(WARNING) << "Tried to remove a socket that no longer exists!";
  } else {
    if (closeOwnedFds) {
      socketHandler->close(it->second);
      auto writeIt = socketWriteFdMap.find(socketId);
      if (writeIt != socketWriteFdMap.end() && writeIt->second != it->second) {
        socketHandler->close(writeIt->second);
      }
    }
    socketFdMap.erase(it);
    socketWriteFdMap.erase(socketId);
  }
}
}  // namespace et
