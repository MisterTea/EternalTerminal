#include "IpcPairServer.hpp"

namespace et {
IpcPairServer::IpcPairServer(shared_ptr<SocketHandler> _socketHandler,
                             const SocketEndpoint &_endpoint)
    : IpcPairEndpoint(_socketHandler, -1), endpoint(_endpoint) {
  serverFd = *(socketHandler->listen(endpoint).begin());
}

IpcPairServer::~IpcPairServer() { ::close(serverFd); }

void IpcPairServer::pollAccept() {
  int fd = socketHandler->accept(serverFd);
  if (fd < 0) {
    // Nothing to accept
    return;
  }

  if (endpointFd >= 0) {
    // Need to disconnect the current client
    closeEndpoint();
  }

  endpointFd = fd;
  recover();
}
}  // namespace et
