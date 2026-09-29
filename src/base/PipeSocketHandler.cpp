#include "PipeSocketHandler.hpp"

#include "RawSocketUtils.hpp"

namespace et {
PipeSocketHandler::PipeSocketHandler() {}

int PipeSocketHandler::connect(const SocketEndpoint& endpoint) {
  lock_guard<std::recursive_mutex> mutexGuard(globalMutex);

  string pipePath = endpoint.name();
  sockaddr_un remote{};

  int sockFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(sockFd);
  string clientPath;
  if (!prepareClientSocket(sockFd, &clientPath)) {
    RawSocketUtils::closeSocket(sockFd);
    return -1;
  }
  remote.sun_family = AF_UNIX;
  strncpy(remote.sun_path, pipePath.c_str(), sizeof(remote.sun_path));

  VLOG(3) << "Connecting to " << endpoint << " with fd " << sockFd;
  int result =
      ::connect(sockFd, (struct sockaddr*)&remote, unixAddressLength(remote));
  auto localErrno = GetErrno();
  VLOG(3) << "AF_UNIX connect returned " << result << " with error "
          << localErrno;
  if (result < 0 && localErrno != EINPROGRESS && localErrno != EWOULDBLOCK) {
    VLOG(3) << "Connection result: " << result << " (" << strerror(localErrno)
            << ")";
    ::shutdown(sockFd, SHUT_RDWR);
    discardClientSocket(sockFd, clientPath);
    sockFd = -1;
    SetErrno(localErrno);
    return sockFd;
  }

  VLOG(4) << "Before waiting on sockFd";
  const bool writable = isSocketWritable(sockFd, 3 /* 3 second timeout */);
  VLOG(3) << "AF_UNIX connect wait returned " << writable;

  if (writable) {
    VLOG(4) << "sockFd " << sockFd << " is writable";
    int so_error;
    socklen_t len = sizeof so_error;

    FATAL_FAIL(
        ::getsockopt(sockFd, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len));
    VLOG(3) << "AF_UNIX connect SO_ERROR is " << so_error;

    if (so_error == 0) {
      LOG(INFO) << "Connected to endpoint " << endpoint;
      // Initialize the socket again once it's blocking to make sure timeouts
      // are set
      initSocket(sockFd);

      // if we get here, we must have connected successfully
    } else {
      LOG(INFO) << "Error connecting to " << endpoint << ": " << so_error << " "
                << strerror(so_error);
      discardClientSocket(sockFd, clientPath);
      sockFd = -1;
    }
  } else {
    auto localErrno = GetErrno();
    LOG(INFO) << "Error connecting to " << endpoint << ": " << localErrno << " "
              << strerror(localErrno);
    discardClientSocket(sockFd, clientPath);
    sockFd = -1;
  }

  LOG(INFO) << sockFd << " is a good socket";
  if (sockFd >= 0) {
    addToActiveSockets(sockFd);
    if (!clientPath.empty()) {
      clientSocketPaths[sockFd] = clientPath;
    }
  }
  return sockFd;
}

set<int> PipeSocketHandler::listen(const SocketEndpoint& endpoint) {
  lock_guard<std::recursive_mutex> guard(globalMutex);

  string pipePath = endpoint.name();
  if (pipeServerSockets.find(pipePath) != pipeServerSockets.end()) {
    throw runtime_error("Tried to listen twice on the same path");
  }

  sockaddr_un local{};

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(fd);
  prepareListenSocket(fd);
  local.sun_family = AF_UNIX; /* local is declared before socket() ^ */
  strncpy(local.sun_path, pipePath.c_str(), sizeof(local.sun_path));
  RawSocketUtils::unlinkSocketPath(local.sun_path);

  FATAL_FAIL(::bind(fd, (struct sockaddr*)&local, unixAddressLength(local)));
  FATAL_FAIL(::listen(fd, 5));
  finishListenSocket(fd, local.sun_path);

  pipeServerSockets[pipePath] = set<int>({fd});
  return pipeServerSockets[pipePath];
}

set<int> PipeSocketHandler::getEndpointFds(const SocketEndpoint& endpoint) {
  lock_guard<std::recursive_mutex> guard(globalMutex);

  string pipePath = endpoint.name();
  if (pipeServerSockets.find(pipePath) == pipeServerSockets.end()) {
    STFATAL << "Tried to getPipeFd on a pipe without calling listen() first: "
            << pipePath;
  }
  return pipeServerSockets[pipePath];
}

void PipeSocketHandler::stopListening(const SocketEndpoint& endpoint) {
  lock_guard<std::recursive_mutex> guard(globalMutex);

  string pipePath = endpoint.name();
  auto it = pipeServerSockets.find(pipePath);
  if (it == pipeServerSockets.end()) {
    STFATAL << "Tried to stop listening to a pipe that we weren't listening on:"
            << pipePath;
  }
  int sockFd = *(it->second.begin());
  FATAL_FAIL(RawSocketUtils::closeSocket(sockFd));
  RawSocketUtils::unlinkSocketPath(pipePath.c_str());
  pipeServerSockets.erase(it);
}

void PipeSocketHandler::close(int fd) {
  string clientPath;
  {
    lock_guard<std::recursive_mutex> guard(globalMutex);
    auto it = clientSocketPaths.find(fd);
    if (it != clientSocketPaths.end()) {
      clientPath = it->second;
      clientSocketPaths.erase(it);
    }
  }
  UnixSocketHandler::close(fd);
  if (!clientPath.empty()) {
    RawSocketUtils::unlinkSocketPath(clientPath.c_str());
  }
}

void PipeSocketHandler::discardClientSocket(int fd, const string& clientPath) {
  FATAL_FAIL(RawSocketUtils::closeSocket(fd));
  if (!clientPath.empty()) {
    RawSocketUtils::unlinkSocketPath(clientPath.c_str());
  }
}
}  // namespace et
