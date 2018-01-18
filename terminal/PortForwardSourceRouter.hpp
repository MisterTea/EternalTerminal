#ifndef __PORT_FORWARD_ROUTER_H__
#define __PORT_FORWARD_ROUTER_H__

#include "Headers.hpp"

#include "PortForwardSourceHandler.hpp"

#include "ETerminal.pb.h"

namespace et {
class PortForwardSourceRouter {
 public:
  PortForwardSourceRouter() {}

  void addListener(shared_ptr<PortForwardSourceHandler> listener);

  void update(vector<PortForwardRequest>* requests,
              vector<PortForwardData>* dataToSend);

  void closeSourceFd(int fd);

  void addSocketId(int socketId, int sourceFd);

  void closeSocketId(int socketId);

  void sendDataOnSocket(int socketId, const string& data);

 protected:
  vector<shared_ptr<PortForwardSourceHandler>> listeners;
  unordered_map<int, shared_ptr<PortForwardSourceHandler>> socketIdListenerMap;
};
}  // namespace et

#endif  // __PORT_FORWARD_ROUTER_H__
