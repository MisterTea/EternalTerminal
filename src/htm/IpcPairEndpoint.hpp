#ifndef __IPC_PAIR_ENDPOINT_H__
#define __IPC_PAIR_ENDPOINT_H__

#include "Headers.hpp"

namespace et {
class IpcPairEndpoint {
 public:
  IpcPairEndpoint(int _endpointFd);
  virtual ~IpcPairEndpoint();
  inline int getEndpointFd() { return endpointFd; }
  virtual void closeEndpoint() {
    ::shutdown(endpointFd, SHUT_RDWR);
    ::close(endpointFd);
    endpointFd = -1;
  }

 protected:
  int endpointFd;
};
}  // namespace et

#endif  // __IPC_PAIR_SERVER_H__