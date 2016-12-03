#ifndef __ETERNAL_TCP_SERVER_CLIENT_CONNECTION__
#define __ETERNAL_TCP_SERVER_CLIENT_CONNECTION__

#include "BackedReader.hpp"
#include "BackedWriter.hpp"

class ServerClientConnection {
public:
  explicit ServerClientConnection(
    const std::shared_ptr<SocketHandler>& _socketHandler,
    int _clientId,
    int _socketFd,
    const string& key
    );

  ~ServerClientConnection() {
    close();
  }

  void close() {
    if (socketFd>0) {
      socketHandler->close(socketFd);
      socketFd = -1;
    }
  }

  void revive(int _socketFd, const std::string &localBuffer) {
    socketFd = _socketFd;
    reader->revive(socketFd, localBuffer);
    writer->revive(socketFd);
  }

  bool hasData();
  ssize_t read(void* buf, size_t count);
  ssize_t readAll(void* buf, size_t count);

  ssize_t write(const void* buf, size_t count);
  void writeAll(const void* buf, size_t count);

  inline shared_ptr<BackedReader> getReader() { return reader; }
  inline shared_ptr<BackedWriter> getWriter() { return writer; }

  inline int getId() { return clientId; }

protected:
  void closeSocket();

  std::shared_ptr<SocketHandler> socketHandler;
  int socketFd;
  int clientId;
  std::shared_ptr<BackedReader> reader;
  std::shared_ptr<BackedWriter> writer;
};

#endif // __ETERNAL_TCP_SERVER_CLIENT_CONNECTION__
