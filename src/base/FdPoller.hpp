#ifndef __ET_FD_POLLER_H__
#define __ET_FD_POLLER_H__

#include "Headers.hpp"

namespace et {
/**
 * @brief Readiness poller backed by epoll on Linux, kqueue on BSD/macOS, and
 * WSAPoll on Windows.
 *
 * Unlike select() there is no FD_SETSIZE ceiling, so a descriptor at or above
 * 1024 is safe to watch.
 */
class FdPoller {
 public:
  /** @brief Descriptors reported ready, split by direction. */
  struct Ready {
    set<int> readable;
    set<int> writable;
  };

  FdPoller();
  ~FdPoller();

  FdPoller(const FdPoller&) = delete;
  FdPoller& operator=(const FdPoller&) = delete;

  /**
   * @brief Replaces the watched set, skipping any descriptor already closed.
   *
   * A descriptor may appear in both sets. Those in `refreshFds` are re-added
   * even when already watched: the poller drops a descriptor once it is
   * closed, so a number a new descriptor has recycled since the last call is
   * otherwise indistinguishable from the one still registered under it.
   */
  void setFds(const set<int>& readFds, const set<int>& writeFds = {},
              const set<int>& refreshFds = {});

  /** @brief Returns the descriptors reported ready, empty on timeout or when
   * interrupted by a signal. */
  Ready wait(int timeoutMs);

 private:
  static constexpr short kRead = 1;
  static constexpr short kWrite = 2;

  bool addFd(int fd, short interest);
  void removeFd(int fd, short interest);
  Ready waitImpl(int capacity, int timeoutMs);

  int pollerFd;
  unordered_map<int, short> registeredFds;
};
}  // namespace et

#endif  // __ET_FD_POLLER_H__
