#ifndef __IPC_PAIR_CLIENT_H__
#define __IPC_PAIR_CLIENT_H__

#include "Headers.hpp"

#include "IpcPairEndpoint.hpp"

namespace et {
class IpcPairClient : public IpcPairEndpoint {
 public:
  IpcPairClient(const string& pipeName);
  virtual ~IpcPairClient() {}

 protected:
};
}  // namespace et

#endif  // __IPC_PAIR_CLIENT_H__