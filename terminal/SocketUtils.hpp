#include "Headers.hpp"

// TODO: Merge this with SocketHandler codebase
int writeAll(int fd, const char* buf, size_t count);

int readAll(int fd, char* buf, size_t count);

template <typename T>
inline T readProto(int fd) {
  T t;
  int64_t length;
  FATAL_FAIL(readAll(fd, (char*)&length, sizeof(int64_t)));
  string s(length, 0);
  FATAL_FAIL(readAll(fd, &s[0], length));
  t.ParseFromString(s);
  return t;
}

template <typename T>
inline void writeProto(int fd, const T& t) {
  string s;
  t.SerializeToString(&s);
  int64_t length = s.length();
  FATAL_FAIL(writeAll(fd, (const char*)&length, sizeof(int64_t)));
  FATAL_FAIL(writeAll(fd, &s[0], length));
}
