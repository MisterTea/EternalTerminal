#include "IpcPairServer.hpp"

namespace et {
IpcPairServer::IpcPairServer(const string &_pipeName)
    : IpcPairEndpoint(-1), pipeName(_pipeName) {
  listen();
}

IpcPairServer::~IpcPairServer() { ::close(serverFd); }

void IpcPairServer::pollAccept() {
  LOG(INFO) << "Listening to id/key FIFO";
  sockaddr_un remote;
  socklen_t t = sizeof(remote);
  int fd = ::accept(serverFd, (struct sockaddr *)&remote, &t);
  if (fd < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      FATAL_FAIL(-1);  // LOG(FATAL) with the error
    } else {
      return;  // Nothing to accept this time
    }
  }

  if (endpointFd >= 0) {
    // Need to disconnect the current client
    closeEndpoint();
  }

  endpointFd = fd;
  // Make sure that socket becomes blocking once it's attached to a client.
  {
    int opts;
    opts = fcntl(endpointFd, F_GETFL);
    FATAL_FAIL(opts);
    opts &= (~O_NONBLOCK);
    FATAL_FAIL(fcntl(endpointFd, F_SETFL, opts));
  }
  recover();
}

void IpcPairServer::listen() {
  sockaddr_un local;

  serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(serverFd);
  local.sun_family = AF_UNIX; /* local is declared before socket() ^ */
  strcpy(local.sun_path, pipeName.c_str());
  unlink(local.sun_path);

  // Also set the accept socket as non-blocking
  {
    int opts;
    opts = fcntl(serverFd, F_GETFL);
    FATAL_FAIL(opts);
    opts |= O_NONBLOCK;
    FATAL_FAIL(fcntl(serverFd, F_SETFL, opts));
  }
  // Also set the accept socket as reusable
  {
    int flag = 1;
    FATAL_FAIL(setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, (char *)&flag,
                          sizeof(int)));
  }

  FATAL_FAIL(::bind(serverFd, (struct sockaddr *)&local, sizeof(sockaddr_un)));
  ::listen(serverFd, 5);
  chmod(local.sun_path, 0777);
}
}  // namespace et