#include "IpcPairEndpoint.hpp"

namespace et {
IpcPairEndpoint::IpcPairEndpoint(shared_ptr<SocketHandler> _socketHandler,
                                 int _endpointFd)
    : socketHandler(_socketHandler), endpointFd(_endpointFd) {}

IpcPairEndpoint::~IpcPairEndpoint() {
  if (endpointFd >= 0) {
    closeEndpoint();
  }
}

}  // namespace et