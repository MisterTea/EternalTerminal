#ifndef __ET_USER_SOCKET_OPS__
#define __ET_USER_SOCKET_OPS__

#include "Headers.hpp"

namespace et {
#ifdef WIN32
/**
 * @brief Create or connect UNIX sockets on Windows.
 *
 * Windows has no uid/gid privilege model, so the `*AsUser` variants perform
 * the operation directly in-process instead of forking a privilege-dropped
 * child (there is nothing to drop to). The API mirrors Unix so the same tests
 * exercise the same socket behavior on every platform.
 */
class UserSocketOps {
 public:
  /**
   * @brief unlink/bind/listen a UNIX socket path.
   * @return Listening fd owned by the caller, or -1 on failure (errno set).
   */
  static int listenUnixAsUser(const string& path, uid_t uid, gid_t gid);

  /**
   * @brief connect() to a UNIX socket path.
   * @return Connected fd owned by the caller, or -1 on failure (errno set).
   */
  static int connectUnixAsUser(const string& path, uid_t uid, gid_t gid);

  /**
   * @brief Create a listening UNIX socket at @p path in the current process.
   * @return Listening fd, or -1 with errno set.
   */
  static int listenAtPath(const string& path);

  /**
   * @brief Connect to a UNIX socket at @p path in the current process.
   * @return Connected fd, or -1 with errno set.
   */
  static int connectAtPath(const string& path);

  /**
   * @brief Flush gcov (when CODE_COVERAGE is on) and _exit.
   *
   * Unix forked children must call this instead of _exit so coverage from the
   * child process is written before the image disappears. Kept on Windows so
   * call sites stay portable; it behaves like _exit.
   */
  static void coverageExit(int code);
};
#else
class UserSocketOps {
 public:
  /**
   * @brief unlink/bind/listen/fchmod a UNIX socket path as @p uid/@p gid.
   * @return Listening fd owned by the caller, or -1 on failure (errno set).
   */
  static int listenUnixAsUser(const string& path, uid_t uid, gid_t gid);

  /**
   * @brief connect() to a UNIX socket path as @p uid/@p gid.
   * @return Connected fd owned by the caller, or -1 on failure (errno set).
   */
  static int connectUnixAsUser(const string& path, uid_t uid, gid_t gid);

  /**
   * @brief Create a listening UNIX socket at @p path in the current process.
   *
   * Used after privilege drop in the forked child, and directly by unit tests.
   * @return Listening fd, or -1 with errno set.
   */
  static int listenAtPath(const string& path);

  /**
   * @brief Connect to a UNIX socket at @p path in the current process.
   * @return Connected fd, or -1 with errno set.
   */
  static int connectAtPath(const string& path);

  /**
   * @brief Flush gcov (when CODE_COVERAGE is on) and _exit.
   *
   * Forked children must call this instead of _exit so coverage from the
   * child process is written before the image disappears.
   */
  static void coverageExit(int code);

 private:
  enum class Op : int { LISTEN = 1, CONNECT = 2 };

  static int runAsUser(Op op, const string& path, uid_t uid, gid_t gid);
  static void childListen(int resultFd, const string& path);
  static void childConnect(int resultFd, const string& path);
  static void sendFd(int channel, int fdToSend, int status, int err);
  static int recvFd(int channel, int* errOut);
};
#endif
}  // namespace et

#endif  // __ET_USER_SOCKET_OPS__
