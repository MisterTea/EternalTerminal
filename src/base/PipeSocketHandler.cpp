#include "PipeSocketHandler.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <resolv.h>
#include <sys/socket.h>
#include <unistd.h>

namespace et {
PipeSocketHandler::PipeSocketHandler() {}

int PipeSocketHandler::connect(const SocketEndpoint& endpoint) {
  lock_guard<std::recursive_mutex> guard(mutex);

  string pipePath = endpoint.name();
  sockaddr_un remote;

  int sockFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(sockFd);
  initSocket(sockFd);
  remote.sun_family = AF_UNIX;
  strcpy(remote.sun_path, pipePath.c_str());

  VLOG(3) << "Connecting to " << endpoint << " with fd " << sockFd;
  int result =
      ::connect(sockFd, (struct sockaddr*)&remote, sizeof(sockaddr_un));
  if (result < 0 && errno != EINPROGRESS) {
    VLOG(3) << "Connection result: " << result << " (" << strerror(errno)
            << ")";
    ::shutdown(sockFd, SHUT_RDWR);
    ::close(sockFd);
    sockFd = -1;
    return sockFd;
  }

  fd_set fdset;
  FD_ZERO(&fdset);
  FD_SET(sockFd, &fdset);
  timeval tv;
  tv.tv_sec = 3; /* 3 second timeout */
  tv.tv_usec = 0;
  VLOG(4) << "Before selecting sockFd";
  select(sockFd + 1, NULL, &fdset, NULL, &tv);

  if (FD_ISSET(sockFd, &fdset)) {
    VLOG(4) << "sockFd " << sockFd << " is selected";
    int so_error;
    socklen_t len = sizeof so_error;

    FATAL_FAIL(::getsockopt(sockFd, SOL_SOCKET, SO_ERROR, &so_error, &len));

    if (so_error == 0) {
      LOG(INFO) << "Connected to endpoint " << endpoint;
      // Initialize the socket again once it's blocking to make sure timeouts
      // are set
      initSocket(sockFd);

      // if we get here, we must have connected successfully
    } else {
      LOG(INFO) << "Error connecting to " << endpoint << ": " << so_error << " "
                << strerror(so_error);
      ::close(sockFd);
      sockFd = -1;
    }
  } else {
    LOG(INFO) << "Error connecting to " << endpoint << ": " << errno << " "
              << strerror(errno);
    ::close(sockFd);
    sockFd = -1;
  }

  LOG(INFO) << sockFd << " is a good socket";
  if (sockFd >= 0) {
    addToActiveSockets(sockFd);
  }
  return sockFd;
}

set<int> PipeSocketHandler::listen(const SocketEndpoint& endpoint) {
  lock_guard<std::recursive_mutex> guard(mutex);

  string pipePath = endpoint.name();
  if (pipeServerSockets.find(pipePath) != pipeServerSockets.end()) {
    throw runtime_error("Tried to listen twice on the same path");
  }

  sockaddr_un local;

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(fd);
  initServerSocket(fd);
  local.sun_family = AF_UNIX; /* local is declared before socket() ^ */
  strcpy(local.sun_path, pipePath.c_str());
  unlink(local.sun_path);

  FATAL_FAIL(::bind(fd, (struct sockaddr*)&local, sizeof(sockaddr_un)));
  ::listen(fd, 5);
  FATAL_FAIL(::chmod(local.sun_path, S_IRUSR | S_IWUSR | S_IXUSR));

  pipeServerSockets[pipePath] = set<int>({fd});
  return pipeServerSockets[pipePath];
}

set<int> PipeSocketHandler::getEndpointFds(const SocketEndpoint& endpoint) {
  lock_guard<std::recursive_mutex> guard(mutex);

  string pipePath = endpoint.name();
  if (pipeServerSockets.find(pipePath) == pipeServerSockets.end()) {
    LOG(FATAL)
        << "Tried to getPipeFd on a pipe without calling listen() first: "
        << pipePath;
  }
  return pipeServerSockets[pipePath];
}

void PipeSocketHandler::stopListening(const SocketEndpoint& endpoint) {
  lock_guard<std::recursive_mutex> guard(mutex);

  string pipePath = endpoint.name();
  auto it = pipeServerSockets.find(pipePath);
  if (it == pipeServerSockets.end()) {
    LOG(FATAL)
        << "Tried to stop listening to a pipe that we weren't listening on:"
        << pipePath;
  }
  int sockFd = *(it->second.begin());
  ::close(sockFd);
}
}  // namespace et
