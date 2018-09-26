#ifndef __ET_SOCKET_ENDPOINT__
#define __ET_SOCKET_ENDPOINT__

#include "Headers.hpp"

namespace et {
class SocketEndpoint {
 public:
  explicit SocketEndpoint(const string &_name) : name(_name), port(-1) {}

  explicit SocketEndpoint(int _port) : name(""), port(_port) {}

  SocketEndpoint(const string &_name, int _port) : name(_name), port(_port) {}

  const string &getName() const { return name; }

  int getPort() const { return port; }

 protected:
  string name;
  int port;
};

inline ostream &operator<<(ostream &os, const SocketEndpoint &self) {
  if (self.getPort() >= 0) {
    return os << self.getName() << ":" << self.getPort(), os;
  } else {
    return os << self.getName(), os;
  }
}
}  // namespace et

#endif  // __ET_SOCKET_ENDPOINT__