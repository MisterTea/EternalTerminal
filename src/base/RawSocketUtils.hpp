#ifndef __ET_RAW_SOCKET_UTILS__
#define __ET_RAW_SOCKET_UTILS__

#include "Headers.hpp"

namespace et {
/**
 * @brief Simple blocking wrappers around POSIX raw socket read/write loops.
 */
class RawSocketUtils {
 public:
  /**
   * @brief Writes the entire buffer to the given descriptor, retrying on EAGAIN.
   */
  static void writeAll(int fd, const char* buf, size_t count);

  /**
   * @brief Reads exactly `count` bytes from the descriptor, waiting for data.
   */
  static void readAll(int fd, char* buf, size_t count);
};
}  // namespace et
#endif  // __ET_RAW_SOCKET_UTILS__
