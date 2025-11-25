#ifndef __DAEMON_CREATOR_H__
#define __DAEMON_CREATOR_H__

#include "Headers.hpp"

namespace et {
/**
 * @brief Helper to daemonize the current process on Unix platforms.
 */
class DaemonCreator {
 public:
  /**
   * @brief Puts the current process into a new session as the session leader.
   * @return 0 on success, -1 when the call fails.
   */
  static int createSessionLeader();

  /**
   * @brief Forks twice, optionally exiting the parent, and redirects stdio/devnull.
   * @param terminateParent Whether the parent should exit immediately after forking.
   * @param childPidFile Optional path to a pid file that is written by the daemon.
   * @return PARENT when running inside the original parent, CHILD inside the daemon.
   */
  static int create(bool terminateParent, string childPidFile);

  /** @brief Returned from `create()` when still running inside the original parent. */
  static const int PARENT = 1;
  /** @brief Returned from `create()` when the call is executing inside the daemon. */
  static const int CHILD = 2;
};
}  // namespace et

#endif  // __DAEMON_CREATOR_H__
