#ifndef __PORT_FORWARD_HANDLER_H__
#define __PORT_FORWARD_HANDLER_H__

#include "ETerminal.pb.h"

#include "Connection.hpp"
#include "ForwardDestinationHandler.hpp"
#include "ForwardSourceHandler.hpp"
#include "SocketHandler.hpp"

namespace et {
class PortForwardHandler {
 public:
  explicit PortForwardHandler(shared_ptr<SocketHandler> _networkSocketHandler,
                              shared_ptr<SocketHandler> _pipeSocketHandler);
  void update(vector<PortForwardDestinationRequest>* requests,
              vector<PortForwardData>* dataToSend);
  void handlePacket(const Packet& packet, shared_ptr<Connection> connection);
  PortForwardSourceResponse createSource(const PortForwardSourceRequest& pfsr,
                                         string* sourceName);
  PortForwardDestinationResponse createDestination(
      const PortForwardDestinationRequest& pfdr);

  void closeSourceFd(int fd);
  void addSourceSocketId(int socketId, int sourceFd);
  void closeSourceSocketId(int socketId);
  void sendDataToSourceOnSocket(int socketId, const string& data);

 protected:
  shared_ptr<SocketHandler> networkSocketHandler;
  shared_ptr<SocketHandler> pipeSocketHandler;
  unordered_map<int, shared_ptr<ForwardDestinationHandler>> destinationHandlers;

  vector<shared_ptr<ForwardSourceHandler>> sourceHandlers;
  unordered_map<int, shared_ptr<ForwardSourceHandler>> socketIdSourceHandlerMap;
};
}  // namespace et

#endif  // __PORT_FORWARD_HANDLER_H__
