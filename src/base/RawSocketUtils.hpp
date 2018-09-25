#ifndef __ET_RAW_SOCKET_UTILS__
#define __ET_RAW_SOCKET_UTILS__

#include "Headers.hpp"

namespace et {
class RawSocketUtils {
 public:
  static void writeAll(int fd, const char* buf, size_t count);

  static void readAll(int fd, char* buf, size_t count);
};
}  // namespace et
#endif  // __ET_RAW_SOCKET_UTILS__
