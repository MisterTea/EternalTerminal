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
   * @brief Writes the entire buffer to the given descriptor, retrying on
   * EAGAIN.
   */
  static void writeAll(int fd, const char* buf, size_t count);

  /**
   * @brief Reads exactly `count` bytes from the descriptor, waiting for data.
   */
  static void readAll(int fd, char* buf, size_t count);

  /**
   * @brief One read(2) from @p fd (recv() on Windows, where descriptors
   * handed around as ints are sockets). Errors are reported via GetErrno().
   */
  static ssize_t readSome(int fd, char* buf, size_t count);

  /** @brief One write(2) to @p fd (send() on Windows). */
  static ssize_t writeSome(int fd, const char* buf, size_t count);

  /** @brief Closes an untracked socket, reporting errors via GetErrno(). */
  static int closeSocket(int fd);

  /** @brief Removes a socket pathname; missing paths are ignored. */
  static void unlinkSocketPath(const string& path);
};
}  // namespace et
#endif  // __ET_RAW_SOCKET_UTILS__
