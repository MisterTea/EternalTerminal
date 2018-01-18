#ifndef __PORT_FORWARD_HANDLER_H__
#define __PORT_FORWARD_HANDLER_H__

#include "PortForwardDestinationHandler.hpp"
#include "ETerminal.pb.h"
#include "ServerClientConnection.hpp"
#include "SocketHandler.hpp"

namespace et {
class PortForwardHandler {
 public:
  explicit PortForwardHandler(shared_ptr<SocketHandler> _socketHandler);
  vector<PortForwardData> update();
  void handlePacket(char packetType, shared_ptr<ServerClientConnection> serverClientState);
  PortForwardResponse createSource(const PortForwardRequest& pfr);

 protected:
  shared_ptr<SocketHandler> socketHandler;
  unordered_map<int, shared_ptr<PortForwardDestinationHandler>> portForwardHandlers;
};
}

#endif // __PORT_FORWARD_HANDLER_H__
