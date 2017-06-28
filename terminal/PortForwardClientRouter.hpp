#ifndef __PORT_FORWARD_ROUTER_H__
#define __PORT_FORWARD_ROUTER_H__

#include "Headers.hpp"

#include "PortForwardClientListener.hpp"

#include "ETerminal.pb.h"

namespace et {
class PortForwardClientRouter {
 public:
  PortForwardClientRouter() {}

  void addListener(shared_ptr<PortForwardClientListener> listener);

  void update(
      vector<PortForwardRequest>* requests,
      vector<PortForwardData>* dataToSend);

  void closeClientFd(int fd);

  void addSocketId(int socketId, int clientFd);

  void closeSocketId(int socketId);

  void sendDataOnSocket(int socketId, const string& data);
 protected:
  vector<shared_ptr<PortForwardClientListener>> listeners;
  unordered_map<int, shared_ptr<PortForwardClientListener>> socketIdListenerMap;
};
}

#endif  // __PORT_FORWARD_ROUTER_H__
