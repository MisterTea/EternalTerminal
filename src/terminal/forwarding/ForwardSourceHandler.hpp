#ifndef __FORWARD_SOURCE_HANDLER_H__
#define __FORWARD_SOURCE_HANDLER_H__

#include "Headers.hpp"

#include "SocketHandler.hpp"

namespace et {
class ForwardSourceHandler {
 public:
  ForwardSourceHandler(shared_ptr<SocketHandler> _socketHandler,
                       const SocketEndpoint& _source,
                       const SocketEndpoint& _destination);

  int listen();

  void update(vector<PortForwardData>* data);

  bool hasUnassignedFd(int fd);

  void closeUnassignedFd(int fd);

  void addSocket(int socketId, int sourceFd);

  void closeSocket(int socketId);

  void sendDataOnSocket(int socketId, const string& data);

  inline SocketEndpoint getDestination() { return destination; }

 protected:
  shared_ptr<SocketHandler> socketHandler;
  SocketEndpoint source;
  SocketEndpoint destination;
  unordered_set<int> unassignedFds;
  unordered_map<int, int> socketFdMap;
};
}  // namespace et

#endif  // __FORWARD_SOURCE_HANDLER_H__
