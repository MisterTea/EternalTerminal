#ifndef __ETERNAL_TCP_CLIENT_CONNECTION__
#define __ETERNAL_TCP_CLIENT_CONNECTION__

#include "Headers.hpp"

#include "Connection.hpp"

namespace et {
extern const int NULL_CLIENT_ID;

class ClientConnection : public Connection {
 public:
  ClientConnection(std::shared_ptr<SocketHandler> _socketHandler,  //
                   const std::string& hostname,                    //
                   int _port,                                      //
                   const string& _id, const string& _key);

  virtual ~ClientConnection();

  void connect();

  virtual void closeSocket();

 protected:
  virtual ssize_t read(string* buf);
  virtual ssize_t write(const string& buf);
  void pollReconnect();

  std::string hostname;
  int port;
  std::shared_ptr<std::thread> reconnectThread;
};
}  // namespace et

#endif  // __ETERNAL_TCP_SERVER_CONNECTION__
