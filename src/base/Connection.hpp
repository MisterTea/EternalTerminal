#ifndef __ET_CONNECTION__
#define __ET_CONNECTION__

#include "Headers.hpp"

#include "BackedReader.hpp"
#include "BackedWriter.hpp"
#include "SocketHandler.hpp"

namespace et {
class Connection {
 public:
  Connection(shared_ptr<SocketHandler> _socketHandler, const string& _id,
             const string& _key);

  virtual ~Connection();

  virtual bool readMessage(Packet* packet);
  virtual void writeMessage(const Packet& packet);

  inline shared_ptr<BackedReader> getReader() { return reader; }
  inline shared_ptr<BackedWriter> getWriter() { return writer; }

  int getSocketFd() { return socketFd; }

  inline shared_ptr<SocketHandler> getSocketHandler() { return socketHandler; }

  bool isDisconnected() { return socketFd == -1; }

  string getId() { return id; }

  inline bool hasData() { return reader->hasData(); }

  virtual void closeSocket();

  void shutdown();

  inline bool isShuttingDown() { return shuttingDown; }

 protected:
  virtual bool read(Packet* packet);
  virtual bool write(const Packet& packet);
  bool recover(int newSocketFd);

  shared_ptr<SocketHandler> socketHandler;
  string id;
  string key;
  std::shared_ptr<BackedReader> reader;
  std::shared_ptr<BackedWriter> writer;
  int socketFd;
  bool shuttingDown;
  recursive_mutex connectionMutex;
};
}  // namespace et

#endif  // __ET_CONNECTION__
