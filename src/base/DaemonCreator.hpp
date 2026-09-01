#ifndef __DAEMON_CREATOR_H__
#define __DAEMON_CREATOR_H__

#include "Headers.hpp"

namespace et {
/**
 * @brief Helper to daemonize or spawn a detached child.
 *
 * On Unix this double-forks and returns CHILD inside the daemon. On Windows it
 * launches `htmd.exe` (next to the current module, or on PATH) with
 * `DETACHED_PROCESS` and always returns PARENT.
 */
class DaemonCreator {
 public:
  /**
   * @brief Puts the current process into a new session as the session leader.
   * @return 0 on success, -1 when the call fails. No-op on Windows.
   */
  static int createSessionLeader();

  /**
   * @brief Forks twice (Unix) or CreateProcess (Windows).
   * @param terminateParent Whether the parent should exit immediately after
   * forking.
   * @param childPidFile Optional path to a pid file that is written by the
   * daemon.
   * @return PARENT when running inside the original parent, CHILD inside the
   * daemon (Unix only).
   */
  static int create(bool terminateParent, string childPidFile);

  /** @brief Returned from `create()` when still running inside the original
   * parent. */
  static const int PARENT = 1;
  /** @brief Returned from `create()` when the call is executing inside the
   * daemon. */
  static const int CHILD = 2;
};
}  // namespace et

#endif  // __DAEMON_CREATOR_H__
