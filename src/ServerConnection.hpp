#ifndef __ETERNAL_TCP_SERVER_CONNECTION__
#define __ETERNAL_TCP_SERVER_CONNECTION__

#include "Headers.hpp"

class ServerConnection {
public:
  explicit ServerConnection(int port);

  void run();

protected:
  int port;
  bool finish;
};


#endif // __ETERNAL_TCP_SERVER_CONNECTION__
