#ifndef __ET_PIPE_SOCKET_HANDLER__
#define __ET_PIPE_SOCKET_HANDLER__

#include "UnixSocketHandler.hpp"

namespace et {
class PipeSocketHandler : public UnixSocketHandler {
 public:
  PipeSocketHandler();
  virtual ~PipeSocketHandler() {}

  virtual int connect(const SocketEndpoint& endpoint);
  virtual set<int> listen(const SocketEndpoint& endpoint);
  virtual set<int> getEndpointFds(const SocketEndpoint& endpoint);
  virtual void stopListening(const SocketEndpoint& endpoint);

 protected:
  map<string, set<int>> pipeServerSockets;

  void initSocket(int fd);
};
}  // namespace et

#endif  // __ET_TCP_SOCKET_HANDLER__
