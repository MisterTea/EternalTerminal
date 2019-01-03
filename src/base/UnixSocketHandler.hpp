#ifndef __ET_UNIX_SOCKET_HANDLER__
#define __ET_UNIX_SOCKET_HANDLER__

#include "SocketHandler.hpp"

namespace et {
class UnixSocketHandler : public SocketHandler {
 public:
  UnixSocketHandler();
  virtual ~UnixSocketHandler() {}

  virtual bool waitForData(int fd, int64_t sec, int64_t usec);
  virtual bool hasData(int fd);
  virtual ssize_t read(int fd, void* buf, size_t count);
  virtual ssize_t write(int fd, const void* buf, size_t count);
  virtual int accept(int fd);
  virtual void close(int fd);
  virtual vector<int> getActiveSockets();

 protected:
  void addToActiveSockets(int fd);
  virtual void initSocket(int fd);
  virtual void initServerSocket(int fd);

  map<int, shared_ptr<recursive_mutex>> activeSocketMutexes;
  recursive_mutex globalMutex;
};
}  // namespace et

#endif  // __ET_UNIX_SOCKET_HANDLER__
