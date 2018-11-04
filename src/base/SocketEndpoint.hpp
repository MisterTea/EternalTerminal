#ifndef __ET_SOCKET_ENDPOINT__
#define __ET_SOCKET_ENDPOINT__

#include "Headers.hpp"

namespace et {
class SocketEndpoint {
 public:
  SocketEndpoint() : name(""), port(-1), is_jumphost(false) {}

  explicit SocketEndpoint(const string &_name)
      : name(_name), port(-1), is_jumphost(false) {}

  explicit SocketEndpoint(const string &_name, bool _is_jumphost)
      : name(_name), port(-1), is_jumphost(_is_jumphost) {}

  explicit SocketEndpoint(int _port)
      : name(""), port(_port), is_jumphost(false) {}

  explicit SocketEndpoint(int _port, bool _is_jumphost)
      : name(""), port(_port), is_jumphost(_is_jumphost) {}

  SocketEndpoint(const string &_name, int _port)
      : name(_name), port(_port), is_jumphost(false) {}

  SocketEndpoint(const string &_name, int _port, bool _is_jumphost)
      : name(_name), port(_port), is_jumphost(_is_jumphost) {}

  const string &getName() const { return name; }

  int getPort() const { return port; }

  bool isJumphost() const { return is_jumphost; }

 protected:
  string name;
  int port;
  bool is_jumphost;
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
