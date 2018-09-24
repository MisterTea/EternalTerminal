#include "IpcPairClient.hpp"

namespace et {
IpcPairClient::IpcPairClient(shared_ptr<SocketHandler> _socketHandler,
                             const SocketEndpoint& endpoint)
    : IpcPairEndpoint(_socketHandler, -1) {
  for (int retry = 0; retry < 5; retry++) {
    endpointFd = socketHandler->connect(endpoint);
    if (endpointFd < 0) {
      sleep(1);
    } else {
      return;
    }
  }
  throw std::runtime_error("Connect to IPC failed");
}

}  // namespace et