#include "IpcPairEndpoint.hpp"

namespace et {
IpcPairEndpoint::IpcPairEndpoint(int _endpointFd) : endpointFd(_endpointFd) {}

IpcPairEndpoint::~IpcPairEndpoint() {
  if (endpointFd >= 0) {
    closeEndpoint();
  }
}

}  // namespace et