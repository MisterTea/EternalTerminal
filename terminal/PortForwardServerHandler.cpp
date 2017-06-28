#include "PortForwardServerHandler.hpp"

namespace et{
PortForwardServerHandler::PortForwardServerHandler(
    shared_ptr<SocketHandler> _socketHandler,
    int _fd,
    int _socketId) :
    socketHandler(_socketHandler),
    fd(_fd),
    socketId(_socketId) {
}

void PortForwardServerHandler::close() {
  socketHandler->close(fd);
}

void PortForwardServerHandler::write(const string& s) {
  socketHandler->writeAllOrReturn(fd, s.c_str(), s.length());
}

void PortForwardServerHandler::update(vector<PortForwardData>* retval) {
  if (fd == -1) {
    return;
  }

  while (socketHandler->hasData(fd)) {
    char buf[1024];
    int bytesRead = socketHandler->read(fd, buf, 1024);
    if (bytesRead == -1 && (errno == EAGAIN ||
                            errno == EWOULDBLOCK)) {
      // Bail for now
      break;
    }
    PortForwardData pwd;
    pwd.set_socketid(socketId);
    if (bytesRead == -1) {
      VLOG(1) << "Got error reading socket " << socketId << " " << strerror(errno);
      pwd.set_error(strerror(errno));
    } else if (bytesRead == 0) {
      VLOG(1) << "Got close reading socket " << socketId;
      pwd.set_closed(true);
    } else {
      VLOG(1) << "Reading " << bytesRead << " bytes from socket " << socketId;
      pwd.set_buffer(string(buf,bytesRead));
    }
    retval->push_back(pwd);
    if (bytesRead < 1) {
      socketHandler->close(fd);
      fd = -1;
      break;
    }
  }
}
}
