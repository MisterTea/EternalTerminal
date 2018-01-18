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
    FATAL_FAIL(readAll(fd, (char*)&length, sizeof(int64_t)));
    if (length < 0 || length > 128 * 1024 * 1024) {
      // If the message is < 0 or too big, assume this is a bad packet and throw
      throw std::runtime_error("Invalid size (<0 or >128 MB)");
    }
    string s(length, 0);
    FATAL_FAIL(readAll(fd, &s[0], length));
    return s;
  }

  static inline void writeMessage(int fd, const string& s) {
    int64_t length = s.length();
    FATAL_FAIL(writeAll(fd, (const char*)&length, sizeof(int64_t)));
    FATAL_FAIL(writeAll(fd, &s[0], length));
  }

  template <typename T>
  static inline T readProto(int fd) {
    T t;
    int64_t length;
    FATAL_FAIL(readAll(fd, (char*)&length, sizeof(int64_t)));
    if (length < 0 || length > 128 * 1024 * 1024) {
      // If the message is < 0 or too big, assume this is a bad packet and throw
      throw std::runtime_error("Invalid size (<0 or >128 MB)");
    }
    string s(length, 0);
    FATAL_FAIL(readAll(fd, &s[0], length));
    if (!t.ParseFromString(s)) {
      throw std::runtime_error("Invalid proto");
    }
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
