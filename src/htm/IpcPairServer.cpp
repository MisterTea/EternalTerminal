#include "IpcPairServer.hpp"

#include "PlatformUtils.hpp"

namespace et {
IpcPairServer::IpcPairServer(shared_ptr<SocketHandler> _socketHandler,
                             const SocketEndpoint& _endpoint)
    : IpcPairEndpoint(_socketHandler, -1), endpoint(_endpoint) {
  serverFd = *(socketHandler->listen(endpoint).begin());
}

IpcPairServer::~IpcPairServer() { socketHandler->stopListening(endpoint); }

void IpcPairServer::pollAccept() {
  int fd = socketHandler->accept(serverFd);
  if (fd < 0) {
    // Nothing to accept
    return;
  }

#ifndef WIN32
  // The socket path is predictable; only this user may drive the daemon.
  uid_t peerUid;
  gid_t peerGid;
  if (!GetPeerCredentials(fd, &peerUid, &peerGid) || peerUid != ::geteuid()) {
    LOG(ERROR) << "Rejecting htm IPC connection from a different user";
    socketHandler->close(fd);
    return;
  }
#endif

  if (endpointFd >= 0) {
    // Need to disconnect the current client
    closeEndpoint();
  }

  endpointFd = fd;
  recover();
}
}  // namespace et
