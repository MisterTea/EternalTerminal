#ifndef __IPC_PAIR_ENDPOINT_H__
#define __IPC_PAIR_ENDPOINT_H__

#include "Headers.hpp"
#include "SocketHandler.hpp"

namespace et {
/**
 * @brief Shared base for HTM IPC endpoints that hold a single pipe descriptor.
 */
class IpcPairEndpoint {
 public:
  IpcPairEndpoint(shared_ptr<SocketHandler> _socketHandler, int _endpointFd);
  virtual ~IpcPairEndpoint();
  inline int getEndpointFd() { return endpointFd; }
  virtual void closeEndpoint() {
    if (endpointFd < 0) {
      return;
    }
    int fd = endpointFd;
    endpointFd = -1;
    try {
      socketHandler->close(fd);
    } catch (const std::exception& ex) {
      LOG(INFO) << "Failed to close endpoint: " << ex.what();
    }
  }

 protected:
  shared_ptr<SocketHandler> socketHandler;
  int endpointFd;
};
}  // namespace et

#endif
