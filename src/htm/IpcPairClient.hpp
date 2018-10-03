#ifndef __IPC_PAIR_CLIENT_H__
#define __IPC_PAIR_CLIENT_H__

#include "Headers.hpp"

#include "IpcPairEndpoint.hpp"
#include "SocketEndpoint.hpp"
#include "SocketHandler.hpp"

namespace et {
class IpcPairClient : public IpcPairEndpoint {
 public:
  IpcPairClient(shared_ptr<SocketHandler> _socketHandler,
                const SocketEndpoint& endpoint);
  virtual ~IpcPairClient() {}

 protected:
};
}  // namespace et

#endif  // __IPC_PAIR_CLIENT_H__