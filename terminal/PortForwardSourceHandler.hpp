#ifndef __PORT_FORWARD_SOURCE_LISTENER_H__
#define __PORT_FORWARD_SOURCE_LISTENER_H__

#include "Headers.hpp"

#include "ETerminal.pb.h"
#include "SocketHandler.hpp"

namespace et {
class PortForwardSourceHandler {
 public:
  PortForwardSourceHandler(shared_ptr<SocketHandler> _socketHandler,
                           int _sourcePort, int _destinationPort);

  int listen();

  void update(vector<PortForwardData>* data);

  bool hasUnassignedFd(int fd);

  void closeUnassignedFd(int fd);

  void addSocket(int socketId, int sourceFd);

  void closeSocket(int socketId);

  void sendDataOnSocket(int socketId, const string& data);

  inline int getDestinationPort() { return destinationPort; }

 protected:
  shared_ptr<SocketHandler> socketHandler;
  int sourcePort;
  int destinationPort;
  unordered_set<int> unassignedFds;
  unordered_map<int, int> socketFdMap;
};
}  // namespace et

#endif  // __PORT_FORWARD_SOURCE_LISTENER_H__
