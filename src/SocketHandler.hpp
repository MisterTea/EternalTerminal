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
    if (length < 0 || length > 128 * 1024 * 1024) {
      // If the message is < 0 or too big, assume this is a bad packet and throw
      throw std::runtime_error("Invalid size (<0 or >128 MB)");
    }
    string s(length, 0);
    readAll(fd, &s[0], length, timeout);
    if (!t.ParseFromString(s)) {
      throw std::runtime_error("Invalid proto");
    }
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
  virtual void listen(int port) = 0;
  virtual set<int> getPortFds(int port) = 0;
  virtual int accept(int fd) = 0;
  virtual void stopListening(int port) = 0;
  virtual void close(int fd) = 0;
};
}  // namespace et

#endif  // __ETERNAL_TCP_SOCKET_HANDLER__
