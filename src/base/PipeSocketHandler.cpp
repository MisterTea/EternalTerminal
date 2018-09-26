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

  string pipePath = endpoint.getName();
  sockaddr_un remote;

  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(fd);
  initSocket(fd);
  remote.sun_family = AF_UNIX;
  strcpy(remote.sun_path, pipePath.c_str());

  VLOG(3) << "Connecting to " << endpoint << " with fd " << fd;
  int result = ::connect(fd, (struct sockaddr *)&remote, sizeof(sockaddr_un));
  VLOG(3) << "Connection result: " << result;
  if (result < 0) {
    ::close(fd);
    fd = -1;
  } else {
    addToActiveSockets(fd);
  }
  return fd;
}

set<int> PipeSocketHandler::listen(const SocketEndpoint& endpoint) {
  lock_guard<std::recursive_mutex> guard(mutex);

  string pipePath = endpoint.getName();
  if (pipeServerSockets.find(pipePath) != pipeServerSockets.end()) {
    LOG(FATAL) << "Tried to listen twice on the same path";
  }

  sockaddr_un local;

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(fd);
  initSocket(fd);
  local.sun_family = AF_UNIX; /* local is declared before socket() ^ */
  strcpy(local.sun_path, pipePath.c_str());
  unlink(local.sun_path);

  // Also set the accept socket as reusable
  {
    int flag = 1;
    FATAL_FAIL(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&flag,
                          sizeof(int)));
  }

  FATAL_FAIL(::bind(fd, (struct sockaddr *)&local, sizeof(sockaddr_un)));
  ::listen(fd, 5);
  chmod(local.sun_path, 0777);

  pipeServerSockets[pipePath] = set<int>({fd});
  return pipeServerSockets[pipePath];
}

set<int> PipeSocketHandler::getEndpointFds(const SocketEndpoint& endpoint) {
  lock_guard<std::recursive_mutex> guard(mutex);

  string pipePath = endpoint.getName();
  if (pipeServerSockets.find(pipePath) == pipeServerSockets.end()) {
    LOG(FATAL)
        << "Tried to getPipeFd on a pipe without calling listen() first: " << pipePath;
  }
  return pipeServerSockets[pipePath];
}

void PipeSocketHandler::stopListening(const SocketEndpoint& endpoint) {
  lock_guard<std::recursive_mutex> guard(mutex);

  string pipePath = endpoint.getName();
  auto it = pipeServerSockets.find(pipePath);
  if (it == pipeServerSockets.end()) {
    LOG(FATAL)
        << "Tried to stop listening to a pipe that we weren't listening on:" << pipePath;
  }
  int sockFd = *(it->second.begin());
  ::close(sockFd);
}

void PipeSocketHandler::initSocket(int fd) {
  int opts;
  opts = fcntl(fd, F_GETFL);
  FATAL_FAIL(opts);
  opts |= O_NONBLOCK;
  FATAL_FAIL(fcntl(fd, F_SETFL, opts));
}
}  // namespace et
