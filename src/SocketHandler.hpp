#ifndef __ETERNAL_TCP_SOCKET_HANDLER__
#define __ETERNAL_TCP_SOCKET_HANDLER__

#include "Headers.hpp"

namespace et {
class SocketHandler {
 public:
  virtual bool hasData(int fd) = 0;
  virtual ssize_t read(int fd, void* buf, size_t count) = 0;
  virtual ssize_t write(int fd, const void* buf, size_t count) = 0;

  void readAll(int fd, void* buf, size_t count);
  void writeAll(int fd, const void* buf, size_t count);

  template <typename T>
  inline T readProto(int fd) {
    T t;
    int64_t length;
    readAll(fd, &length, sizeof(int64_t));
    string s(length, 0);
    readAll(fd, &s[0], length);
    t.ParseFromString(s);
    return t;
  }

  template <typename T>
  inline void writeProto(int fd, const T& t) {
    string s;
    t.SerializeToString(&s);
    int64_t length = s.length();
    writeAll(fd, &length, sizeof(int64_t));
    writeAll(fd, &s[0], length);
  }

  virtual int connect(const std::string& hostname, int port) = 0;
  virtual int listen(int port) = 0;
  virtual void stopListening() = 0;
  virtual void close(int fd) = 0;
};
}

#endif  // __ETERNAL_TCP_SOCKET_HANDLER__
