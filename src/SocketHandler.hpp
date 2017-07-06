#ifndef __ETERNAL_TCP_SOCKET_HANDLER__
#define __ETERNAL_TCP_SOCKET_HANDLER__

#include "Headers.hpp"

namespace et {
class SocketHandler {
 public:
  virtual bool hasData(int fd) = 0;
  virtual ssize_t read(int fd, void* buf, size_t count) = 0;
  virtual ssize_t write(int fd, const void* buf, size_t count) = 0;

  void readAll(int fd, void* buf, size_t count, bool timeout);
  int writeAllOrReturn(int fd, const void* buf, size_t count);
  void writeAllOrThrow(int fd, const void* buf, size_t count, bool timeout);

  template <typename T>
  inline T readProto(int fd, bool timeout) {
    T t;
    int64_t length;
    readAll(fd, &length, sizeof(int64_t), timeout);
    string s(length, 0);
    readAll(fd, &s[0], length, timeout);
    t.ParseFromString(s);
    return t;
  }

  template <typename T>
  inline void writeProto(int fd, const T& t, bool timeout) {
    string s;
    t.SerializeToString(&s);
    int64_t length = s.length();
    writeAllOrThrow(fd, &length, sizeof(int64_t), timeout);
    writeAllOrThrow(fd, &s[0], length, timeout);
  }

  virtual int connect(const std::string& hostname, int port) = 0;
  virtual int listen(int port) = 0;
  virtual void stopListening(int port) = 0;
  virtual void close(int fd) = 0;
};
}  // namespace et

#endif  // __ETERNAL_TCP_SOCKET_HANDLER__
