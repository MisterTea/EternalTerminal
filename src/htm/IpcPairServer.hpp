#ifndef __IPC_PAIR_SERVER_H__
#define __IPC_PAIR_SERVER_H__

#include "Headers.hpp"

#include "IpcPairEndpoint.hpp"

namespace et {
class IpcPairServer : public IpcPairEndpoint {
 public:
  IpcPairServer(const string& _pipeName);
  virtual ~IpcPairServer();
  virtual void pollAccept();
  inline int getServerFd() { return serverFd; }

  virtual void recover() = 0;

 protected:
  void listen();
  int serverFd;
  string pipeName;
};
}  // namespace et

#endif  // __IPC_PAIR_SERVER_H__