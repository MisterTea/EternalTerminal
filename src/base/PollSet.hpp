#ifndef __ET_POLL_SET__
#define __ET_POLL_SET__

#include "Headers.hpp"

namespace et {
/**
 * @brief One-shot readiness wait over a small, per-iteration set of fds.
 *
 * Backed by poll() on Unix and WSAPoll() on Windows (PollSetUnix.cpp,
 * PollSetWindows.cpp). Unlike FdPoller it keeps no kernel state and, unlike
 * epoll, accepts the regular file nohup(1) leaves on the console descriptor.
 */
class PollSet {
 public:
  /**
   * @brief Adds interest in @p fd, merging with an existing entry. Negative
   * fds are ignored so callers can pass "no descriptor" through.
   */
  void watch(int fd, bool read, bool write);

  /**
   * @brief Waits up to @p timeoutMs for any watched fd.
   *
   * Sleeps for the timeout when nothing is watched so callers keep their
   * cadence.
   * @return The fds watched for read that are readable, hung up or in error.
   */
  set<int> waitReadable(int timeoutMs);

 private:
  struct Entry {
    int fd;
    bool read;
    bool write;
  };
  vector<Entry> entries;
};
}  // namespace et

#endif  // __ET_POLL_SET__
