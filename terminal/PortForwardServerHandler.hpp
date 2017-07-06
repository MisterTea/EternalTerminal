#ifndef __PORT_FORWARD_SERVER_HANDLER_H__
#define __PORT_FORWARD_SERVER_HANDLER_H__

#include "Headers.hpp"

#include "SocketHandler.hpp"

#include "ETerminal.pb.h"

namespace et {
class PortForwardServerHandler {
 public:
  PortForwardServerHandler(shared_ptr<SocketHandler> _socketHandler, int _fd,
                           int _socketId);

  void write(const string& s);

  void update(vector<PortForwardData>* retval);

  void close();

  inline int getFd() { return fd; }

 protected:
  shared_ptr<SocketHandler> socketHandler;
  int fd;
  int socketId;
};
}  // namespace et

#endif  // __PORT_FORWARD_SERVER_HANDLER_H__
