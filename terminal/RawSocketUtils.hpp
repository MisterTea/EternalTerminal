#ifndef __ETERNAL_TCP_RAW_SOCKET_UTILS__
#define __ETERNAL_TCP_RAW_SOCKET_UTILS__

#include "Headers.hpp"

namespace et {
class RawSocketUtils {
 public:
  static int writeAll(int fd, const char* buf, size_t count);

  static int readAll(int fd, char* buf, size_t count);

  static inline string readMessage(int fd) {
    int64_t length;
    readAll(fd, (char*)&length, sizeof(int64_t));
    string s(length, 0);
    readAll(fd, &s[0], length);
    return s;
  }

  static inline void writeMessage(int fd, const string& s) {
    int64_t length = s.length();
    writeAll(fd, (const char*)&length, sizeof(int64_t));
    writeAll(fd, &s[0], length);
  }

  template <typename T>
  static inline T readProto(int fd) {
    T t;
    int64_t length;
    FATAL_FAIL(readAll(fd, (char*)&length, sizeof(int64_t)));
    string s(length, 0);
    FATAL_FAIL(readAll(fd, &s[0], length));
    t.ParseFromString(s);
    return t;
  }

  template <typename T>
  static inline void writeProto(int fd, const T& t) {
    string s;
    t.SerializeToString(&s);
    int64_t length = s.length();
    FATAL_FAIL(writeAll(fd, (const char*)&length, sizeof(int64_t)));
    FATAL_FAIL(writeAll(fd, &s[0], length));
  }
};
}  // namespace et
#endif  // __ETERNAL_TCP_RAW_SOCKET_UTILS__
