#ifndef __IPC_PAIR_SERVER_H__
#define __IPC_PAIR_SERVER_H__

#include "Headers.hpp"
#include "IpcPairEndpoint.hpp"

namespace et {
class IpcPairServer : public IpcPairEndpoint {
 public:
  IpcPairServer(shared_ptr<SocketHandler> _socketHandler,
                const SocketEndpoint &_endpoint);
  virtual ~IpcPairServer();
  virtual void pollAccept();
  inline int getServerFd() { return serverFd; }

  virtual void recover() = 0;

 protected:
  int serverFd;
  SocketEndpoint endpoint;
};
}  // namespace et

#endif  // __IPC_PAIR_SERVER_H__