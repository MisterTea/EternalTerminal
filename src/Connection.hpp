#ifndef __ETERNAL_TCP_CONNECTION__
#define __ETERNAL_TCP_CONNECTION__

#include "Headers.hpp"

#include "BackedReader.hpp"
#include "BackedWriter.hpp"
#include "SocketHandler.hpp"

namespace et {
class Connection {
 public:
  Connection(std::shared_ptr<SocketHandler> _socketHandler, const string& key);

  virtual ~Connection();

  virtual bool readMessage(string* buf);
  virtual void writeMessage(const string& message);

  template <typename T>
  inline T readProto() {
    T t;
    string s;
    ssize_t readMessages = readMessage(&s);
    if (readMessages) {
      t.ParseFromString(s);
    }
    return t;
  }

  template <typename T>
  inline void writeProto(const T& t) {
    string s;
    t.SerializeToString(&s);
    writeMessage(s);
  }

  inline shared_ptr<BackedReader> getReader() { return reader; }
  inline shared_ptr<BackedWriter> getWriter() { return writer; }

  int getSocketFd() { return socketFd; }

  bool isDisconnected() { return socketFd == -1; }

  int getClientId() { return clientId; }

  inline bool hasData() { return reader->hasData(); }

  virtual void closeSocket();

  void shutdown();

 protected:
  virtual ssize_t read(string* buf);
  virtual ssize_t write(const string& buf);
  bool recover(int newSocketFd);

  shared_ptr<SocketHandler> socketHandler;
  string key;
  std::shared_ptr<BackedReader> reader;
  std::shared_ptr<BackedWriter> writer;
  int socketFd;
  int clientId;
  bool shuttingDown;
};
}

#endif  // __ETERNAL_TCP_CONNECTION__
