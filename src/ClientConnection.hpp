#ifndef __ETERNAL_TCP_CLIENT_CONNECTION__
#define __ETERNAL_TCP_CLIENT_CONNECTION__

#include "Headers.hpp"

#include "SocketHandler.hpp"
#include "BackedReader.hpp"
#include "BackedWriter.hpp"

extern const int NULL_CLIENT_ID;

class ClientConnection {
public:
  explicit ClientConnection(
    std::shared_ptr<SocketHandler> _socketHandler,
    const std::string& hostname,
    int port,
    const string& key
    );

  ~ClientConnection();

  void connect();

  int getSocketFd() {
    return socketFd;
  }
  int getClientId() {
    return clientId;
  }

  bool hasData();
  ssize_t read(void* buf, size_t count);
  ssize_t readAll(void* buf, size_t count);

  ssize_t write(const void* buf, size_t count);
  void writeAll(const void* buf, size_t count);
protected:
  void closeSocket();
  void pollReconnect();

  std::shared_ptr<SocketHandler> socketHandler;
  std::string hostname;
  int port;
  string key;
  int socketFd;
  int clientId;
  std::shared_ptr<BackedReader> reader;
  std::shared_ptr<BackedWriter> writer;
  std::shared_ptr<std::thread> reconnectThread;
};

#endif // __ETERNAL_TCP_SERVER_CONNECTION__
