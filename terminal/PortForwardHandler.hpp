#ifndef __PORT_FORWARD_HANDLER_H__
#define __PORT_FORWARD_HANDLER_H__

#include "ETerminal.pb.h"

#include "PortForwardSourceHandler.hpp"
#include "PortForwardDestinationHandler.hpp"
#include "ServerClientConnection.hpp"
#include "SocketHandler.hpp"

namespace et {
class PortForwardHandler {
 public:
  explicit PortForwardHandler(shared_ptr<SocketHandler> _socketHandler);
  void update(
      vector<PortForwardRequest>* requests,
      vector<PortForwardData>* dataToSend);
  void handlePacket(char packetType, shared_ptr<ServerClientConnection> serverClientState);
  PortForwardResponse createDestination(const PortForwardRequest& pfr);

  void addSourceHandler(shared_ptr<PortForwardSourceHandler> handler);
  void closeSourceFd(int fd);
  void addSourceSocketId(int socketId, int sourceFd);
  void closeSourceSocketId(int socketId);
  void sendDataToSourceOnSocket(int socketId, const string& data);

 protected:
  shared_ptr<SocketHandler> socketHandler;
  unordered_map<int, shared_ptr<PortForwardDestinationHandler>> portForwardHandlers;

  vector<shared_ptr<PortForwardSourceHandler>> handlers;
  unordered_map<int, shared_ptr<PortForwardSourceHandler>> socketIdSourceHandlerMap;
};
}

#endif // __PORT_FORWARD_HANDLER_H__
