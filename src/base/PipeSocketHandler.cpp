#include "PipeSocketHandler.hpp"

#ifndef WIN32
#include "UserSocketOps.hpp"
#else
#include <io.h>
#endif

namespace et {
PipeSocketHandler::PipeSocketHandler() {}

int PipeSocketHandler::connect(const SocketEndpoint& endpoint) {
  lock_guard<std::recursive_mutex> mutexGuard(globalMutex);

  string pipePath = endpoint.name();
  sockaddr_un remote{};

  int sockFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(sockFd);
#ifndef WIN32
  initSocket(sockFd);
#else
  // Windows AF_UNIX does not autobind clients. bind/connect must also be the
  // first socket operation, so bind a short, unique pathname before applying
  // non-blocking configuration.
  string clientPath = "htmc." + to_string(GetCurrentProcessId()) + "." +
                      to_string(GetTickCount64()) + "." + to_string(sockFd);
  sockaddr_un client;
  ZeroMemory(&client, sizeof(client));
  client.sun_family = AF_UNIX;
  strncpy_s(client.sun_path, sizeof(client.sun_path), clientPath.c_str(),
            _TRUNCATE);
  DeleteFileA(clientPath.c_str());
  if (::bind(sockFd, reinterpret_cast<sockaddr*>(&client), sizeof(client)) <
      0) {
    ::closesocket(sockFd);
    return -1;
  }
#endif
  remote.sun_family = AF_UNIX;
  strncpy(remote.sun_path, pipePath.c_str(), sizeof(remote.sun_path));

  VLOG(3) << "Connecting to " << endpoint << " with fd " << sockFd;
  int result =
      ::connect(sockFd, (struct sockaddr*)&remote, sizeof(sockaddr_un));
  auto localErrno = GetErrno();
  VLOG(3) << "AF_UNIX connect returned " << result << " with error "
          << localErrno;
  if (result < 0 && localErrno != EINPROGRESS && localErrno != EWOULDBLOCK) {
    VLOG(3) << "Connection result: " << result << " (" << strerror(localErrno)
            << ")";
#ifdef WIN32
    ::shutdown(sockFd, SD_BOTH);
#else
    ::shutdown(sockFd, SHUT_RDWR);
#endif
#ifdef _MSC_VER
    FATAL_FAIL(::closesocket(sockFd));
    DeleteFileA(clientPath.c_str());
#else
    FATAL_FAIL(::close(sockFd));
#endif
    sockFd = -1;
    SetErrno(localErrno);
    return sockFd;
  }

  fd_set fdset;
  FD_ZERO(&fdset);
  FD_SET(sockFd, &fdset);
  timeval tv;
  tv.tv_sec = 3; /* 3 second timeout */
  tv.tv_usec = 0;
  VLOG(4) << "Before selecting sockFd";
  int selectResult = select(sockFd + 1, NULL, &fdset, NULL, &tv);
  VLOG(3) << "AF_UNIX connect select returned " << selectResult;

  if (FD_ISSET(sockFd, &fdset)) {
    VLOG(4) << "sockFd " << sockFd << " is selected";
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
#ifdef _MSC_VER
      FATAL_FAIL(::closesocket(sockFd));
      DeleteFileA(clientPath.c_str());
#else
      FATAL_FAIL(::close(sockFd));
#endif
      sockFd = -1;
    }
  } else {
    auto localErrno = GetErrno();
    LOG(INFO) << "Error connecting to " << endpoint << ": " << localErrno << " "
              << strerror(localErrno);
#ifdef _MSC_VER
    FATAL_FAIL(::closesocket(sockFd));
    DeleteFileA(clientPath.c_str());
#else
    FATAL_FAIL(::close(sockFd));
#endif
    sockFd = -1;
  }

  LOG(INFO) << sockFd << " is a good socket";
  if (sockFd >= 0) {
    addToActiveSockets(sockFd);
#ifdef WIN32
    clientSocketPaths[sockFd] = clientPath;
#endif
  }
  return sockFd;
}

#ifndef WIN32
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
#endif

set<int> PipeSocketHandler::listen(const SocketEndpoint& endpoint) {
  lock_guard<std::recursive_mutex> guard(globalMutex);

  string pipePath = endpoint.name();
  if (pipeServerSockets.find(pipePath) != pipeServerSockets.end()) {
    throw runtime_error("Tried to listen twice on the same path");
  }

  sockaddr_un local{};

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(fd);
#ifndef WIN32
  initServerSocket(fd);
#endif
  local.sun_family = AF_UNIX; /* local is declared before socket() ^ */
  strncpy(local.sun_path, pipePath.c_str(), sizeof(local.sun_path));
#ifdef WIN32
  _unlink(local.sun_path);
#else
  unlink(local.sun_path);
#endif

  FATAL_FAIL(::bind(fd, (struct sockaddr*)&local, sizeof(sockaddr_un)));
  FATAL_FAIL(::listen(fd, 5));
#ifdef WIN32
  // bind must be the first operation on a Windows AF_UNIX socket. Configure
  // non-blocking mode only after the address family provider is selected.
  initSocket(fd);
#endif
#ifndef WIN32
  FATAL_FAIL(::chmod(local.sun_path, S_IRUSR | S_IWUSR | S_IXUSR));
#endif

  pipeServerSockets[pipePath] = set<int>({fd});
  return pipeServerSockets[pipePath];
}

#ifndef WIN32
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
#endif

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
#ifdef _MSC_VER
  FATAL_FAIL(::closesocket(sockFd));
#else
  FATAL_FAIL(::close(sockFd));
#endif
#ifdef WIN32
  _unlink(pipePath.c_str());
#else
  ::unlink(pipePath.c_str());
#endif
  pipeServerSockets.erase(it);
}

void PipeSocketHandler::close(int fd) {
#ifdef WIN32
  string clientPath;
  {
    lock_guard<std::recursive_mutex> guard(globalMutex);
    auto it = clientSocketPaths.find(fd);
    if (it != clientSocketPaths.end()) {
      clientPath = it->second;
      clientSocketPaths.erase(it);
    }
  }
#endif
  UnixSocketHandler::close(fd);
#ifdef WIN32
  if (!clientPath.empty()) {
    DeleteFileA(clientPath.c_str());
  }
#endif
}

void PipeSocketHandler::minimizeKernelBuffering(int fd) {
#ifndef WIN32
  // Bound the kernel buffer on this unix socket. After a Ctrl+C flush of
  // the server WriteBuffer, leftover local backlog would otherwise still
  // drain to the client. 64KB does not limit throughput on a local socket.
  int sndbuf = 64 * 1024;
  if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf, sizeof(sndbuf)) <
      0) {
    LOG(WARNING) << "Failed to set SO_SNDBUF: " << strerror(errno);
  }
#endif
}
}  // namespace et
