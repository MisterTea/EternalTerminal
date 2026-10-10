#include "IpcPairClient.hpp"

#include "PlatformUtils.hpp"

namespace et {
IpcPairClient::IpcPairClient(shared_ptr<SocketHandler> _socketHandler,
                             const SocketEndpoint& endpoint)
    : IpcPairEndpoint(_socketHandler, -1) {
  for (int retry = 0; retry < 5; retry++) {
    endpointFd = socketHandler->connect(endpoint);
    if (endpointFd < 0) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    } else {
#ifndef WIN32
      // Another user can bind the predictable path first; refuse to send
      // our keystrokes to it.
      uid_t peerUid;
      gid_t peerGid;
      if (!GetPeerCredentials(endpointFd, &peerUid, &peerGid) ||
          peerUid != ::geteuid()) {
        closeEndpoint();
        throw std::runtime_error("HTM IPC server belongs to a different user");
      }
#endif
      return;
    }
  }
  throw std::runtime_error("Connect to IPC failed");
}

}  // namespace et