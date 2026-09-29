#include "FdPoller.hpp"

namespace et {
void FdPoller::setFds(const set<int>& readFds, const set<int>& writeFds,
                      const set<int>& refreshFds) {
  auto interestFor = [&readFds, &writeFds](int fd) -> short {
    short interest = 0;
    if (readFds.count(fd) != 0) {
      interest |= kRead;
    }
    if (writeFds.count(fd) != 0) {
      interest |= kWrite;
    }
    return interest;
  };

  for (const auto& [fd, interest] : registeredFds) {
    if (interestFor(fd) != interest || refreshFds.count(fd) != 0) {
      // Removed descriptors may already be closed and their numbers reused
      // by another thread. Rebuild without operating on those stale numbers.
      reset();
      registeredFds.clear();
      break;
    }
  }

  set<int> allFds = readFds;
  allFds.insert(writeFds.begin(), writeFds.end());
  for (int fd : allFds) {
    if (registeredFds.count(fd) == 0 && addFd(fd, interestFor(fd))) {
      registeredFds[fd] = interestFor(fd);
    }
  }
}

FdPoller::Ready FdPoller::wait(int timeoutMs) {
  // kqueue reports each filter separately, so a descriptor watched both ways
  // yields two events. epoll_wait() rejects a zero-sized buffer and kevent()
  // would return without waiting, hence the floor of one.
  return waitImpl(static_cast<int>(max<size_t>(registeredFds.size() * 2, 1)),
                  timeoutMs);
}
}  // namespace et
