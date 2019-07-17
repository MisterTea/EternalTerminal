#ifndef __IPC_PAIR_ENDPOINT_H__
#define __IPC_PAIR_ENDPOINT_H__

#include "Headers.hpp"

#include "HtmHeaderCodes.hpp"
#include "SocketHandler.hpp"

namespace et {
class IpcPairEndpoint {
 public:
  IpcPairEndpoint(shared_ptr<SocketHandler> _socketHandler, int _endpointFd);
  virtual ~IpcPairEndpoint();
  inline int getEndpointFd() { return endpointFd; }
  virtual void closeEndpoint() {
    unsigned char header = SESSION_END;
    socketHandler->writeAllOrThrow(endpointFd, (const char *)&header, 1, false);
    socketHandler->close(endpointFd);
    endpointFd = -1;
  }

 protected:
  shared_ptr<SocketHandler> socketHandler;
  int endpointFd;
};
}  // namespace et

#endif  // __IPC_PAIR_SERVER_H__