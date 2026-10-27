#include "PipeSocketHandler.hpp"
#include "UserSocketOps.hpp"

namespace et {
bool PipeSocketHandler::prepareClientSocket(int fd, string* /*clientPath*/) {
  initSocket(fd);
  return true;
}

void PipeSocketHandler::prepareListenSocket(int fd) { initServerSocket(fd); }

void PipeSocketHandler::finishListenSocket(int /*fd*/, const string& pipePath) {
  FATAL_FAIL(::chmod(pipePath.c_str(), S_IRUSR | S_IWUSR | S_IXUSR));
}

int PipeSocketHandler::connectAsUser(const SocketEndpoint& endpoint, uid_t uid,
                                     gid_t gid) {
  lock_guard<std::recursive_mutex> mutexGuard(globalMutex);

  string pipePath = endpoint.name();
  VLOG(3) << "Connecting to " << endpoint << " as uid " << uid;
  int sockFd = UserSocketOps::connectUnixAsUser(pipePath, uid, gid);
  if (sockFd < 0) {
    return -1;
  }
  initSocket(sockFd);
  addToActiveSockets(sockFd);
  LOG(INFO) << "Connected to endpoint " << endpoint << " as uid " << uid;
  return sockFd;
}

set<int> PipeSocketHandler::listenAsUser(const SocketEndpoint& endpoint,
                                         uid_t uid, gid_t gid) {
  lock_guard<std::recursive_mutex> guard(globalMutex);

  string pipePath = endpoint.name();
  if (pipeServerSockets.find(pipePath) != pipeServerSockets.end()) {
    throw runtime_error("Tried to listen twice on the same path");
  }

  int fd = UserSocketOps::listenUnixAsUser(pipePath, uid, gid);
  if (fd < 0) {
    throw runtime_error(string("Failed to listen as user on ") + pipePath +
                        ": " + strerror(GetErrno()));
  }
  initServerSocket(fd);
  pipeServerSockets[pipePath] = set<int>({fd});
  return pipeServerSockets[pipePath];
}

void PipeSocketHandler::minimizeKernelBuffering(int fd) {
  // Bound the kernel buffer on this unix socket. After a Ctrl+C flush of
  // the server WriteBuffer, leftover local backlog would otherwise still
  // drain to the client. 64KB does not limit throughput on a local socket.
  int sndbuf = 64 * 1024;
  if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf, sizeof(sndbuf)) <
      0) {
    LOG(WARNING) << "Failed to set SO_SNDBUF: " << strerror(errno);
  }
}
}  // namespace et
