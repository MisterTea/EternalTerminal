#ifndef __PORT_FORWARD_HANDLER_H__
#define __PORT_FORWARD_HANDLER_H__

#include "ETerminal.pb.h"

#include "Connection.hpp"
#include "PortForwardDestinationHandler.hpp"
#include "PortForwardSourceHandler.hpp"
#include "SocketHandler.hpp"

namespace et {
class PortForwardHandler {
 public:
  explicit PortForwardHandler(shared_ptr<SocketHandler> _socketHandler);
  void update(vector<PortForwardDestinationRequest>* requests,
              vector<PortForwardData>* dataToSend);
  void handlePacket(const Packet& packet, shared_ptr<Connection> connection);
  PortForwardSourceResponse createSource(const PortForwardSourceRequest& pfsr);
  PortForwardDestinationResponse createDestination(
      const PortForwardDestinationRequest& pfdr);

  void closeSourceFd(int fd);
  void addSourceSocketId(int socketId, int sourceFd);
  void closeSourceSocketId(int socketId);
  void sendDataToSourceOnSocket(int socketId, const string& data);

 protected:
  shared_ptr<SocketHandler> socketHandler;
  unordered_map<int, shared_ptr<PortForwardDestinationHandler>>
      destinationHandlers;

  vector<shared_ptr<PortForwardSourceHandler>> sourceHandlers;
  unordered_map<int, shared_ptr<PortForwardSourceHandler>>
      socketIdSourceHandlerMap;
};
}  // namespace et

#endif  // __PORT_FORWARD_HANDLER_H__
