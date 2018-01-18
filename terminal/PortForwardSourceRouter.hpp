#ifndef __PORT_FORWARD_ROUTER_H__
#define __PORT_FORWARD_ROUTER_H__

#include "Headers.hpp"

#include "PortForwardSourceHandler.hpp"

#include "ETerminal.pb.h"

namespace et {
class PortForwardSourceRouter {
 public:
  PortForwardSourceRouter() {}

  void addSourceHandler(shared_ptr<PortForwardSourceHandler> handler);

  void update(vector<PortForwardRequest>* requests,
              vector<PortForwardData>* dataToSend);

  void closeSourceFd(int fd);

  void addSocketId(int socketId, int sourceFd);

  void closeSocketId(int socketId);

  void sendDataOnSocket(int socketId, const string& data);

 protected:
  vector<shared_ptr<PortForwardSourceHandler>> handlers;
  unordered_map<int, shared_ptr<PortForwardSourceHandler>> socketIdSourceHandlerMap;
};
}  // namespace et

#endif  // __PORT_FORWARD_ROUTER_H__
