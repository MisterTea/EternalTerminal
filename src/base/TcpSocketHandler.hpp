#ifndef __ET_TCP_SOCKET_HANDLER__
#define __ET_TCP_SOCKET_HANDLER__

#include "UnixSocketHandler.hpp"

namespace et {
class TcpSocketHandler : public UnixSocketHandler {
 public:
  TcpSocketHandler();
  virtual ~TcpSocketHandler() {}

  virtual int connect(const SocketEndpoint& endpoint);
  virtual set<int> listen(const SocketEndpoint& endpoint);
  virtual set<int> getEndpointFds(const SocketEndpoint& endpoint);
  virtual void stopListening(const SocketEndpoint& endpoint);

 protected:
  map<int, set<int>> portServerSockets;

  virtual void initSocket(int fd);
};
}  // namespace et

#endif  // __ET_TCP_SOCKET_HANDLER__
