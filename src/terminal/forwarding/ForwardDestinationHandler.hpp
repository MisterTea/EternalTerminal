#ifndef __PORT_FORWARD_DESTINATION_HANDLER_H__
#define __PORT_FORWARD_DESTINATION_HANDLER_H__

#include "ETerminal.pb.h"
#include "Headers.hpp"
#include "SocketHandler.hpp"

namespace et {
/**
 * @brief Writes port-forwarded data to a destination socket (server side).
 */
class ForwardDestinationHandler {
 public:
  /** @brief Binds the handler to a destination fd so data can be sent downstream. */
  ForwardDestinationHandler(shared_ptr<SocketHandler> _socketHandler, int _fd,
                            int _socketId);
  /** @brief Sends bytes that need to travel to the destination socket. */
  void write(const string& s);

  /** @brief Polls for incoming data to send back to the source. */
  void update(vector<PortForwardData>* retval);

  /** @brief Closes the destination socket and marks the handler inactive. */
  void close();

  /** @brief Accessor for the wrapped destination descriptor. */
  inline int getFd() { return fd; }

 protected:
  /** @brief Socket helper that drives the destination endpoint. */
  shared_ptr<SocketHandler> socketHandler;
  /** @brief File descriptor for the outbound destination. */
  int fd;
  /** @brief Logical identifier supplied over the control channel. */
  int socketId;
};
}  // namespace et

#endif  // __PORT_FORWARD_DESTINATION_HANDLER_H__
