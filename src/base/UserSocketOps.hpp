#ifndef __ET_USER_SOCKET_OPS__
#define __ET_USER_SOCKET_OPS__

#include "Headers.hpp"

namespace et {
#ifndef WIN32
/**
 * @brief Create or connect UNIX sockets after dropping to a session uid/gid.
 *
 * etserver is multithreaded and cannot safely seteuid in-process. These helpers
 * fork a child, drop privileges, perform the socket operation, and return the
 * resulting fd to the parent via SCM_RIGHTS.
 */
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
