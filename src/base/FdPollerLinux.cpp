#include <sys/epoll.h>

#include "FdPoller.hpp"

namespace et {
FdPoller::FdPoller() : pollerFd(-1) { reset(); }

void FdPoller::reset() {
  const int newPollerFd = epoll_create1(EPOLL_CLOEXEC);
  if (newPollerFd < 0) {
    throw runtime_error(string("epoll_create1 failed: ") + strerror(errno));
  }
  if (pollerFd >= 0) {
    ::close(pollerFd);
  }
  pollerFd = newPollerFd;
}

FdPoller::~FdPoller() { ::close(pollerFd); }

FdPoller::Ready FdPoller::waitImpl(int capacity, int timeoutMs) {
  vector<epoll_event> events(capacity);
  int count = epoll_wait(pollerFd, events.data(), capacity, timeoutMs);
  if (count < 0) {
    if (errno == EINTR) {
      return {};
    }
    throw runtime_error(string("epoll_wait failed: ") + strerror(errno));
  }

  Ready ready;
  for (int i = 0; i < count; ++i) {
    const int fd = events[i].data.fd;
    const uint32_t reported = events[i].events;
    // EPOLLHUP and EPOLLERR arrive unrequested, so report them only in the
    // direction the caller asked about.
    auto it = registeredFds.find(fd);
    const short interest =
        it == registeredFds.end() ? kRead | kWrite : it->second;
    if ((interest & kRead) != 0 &&
        (reported & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0) {
      ready.readable.insert(fd);
    }
    if ((interest & kWrite) != 0 &&
        (reported & (EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0) {
      ready.writable.insert(fd);
    }
  }
  return ready;
}

bool FdPoller::addFd(int fd, short interest) {
  // reset() can reuse a closed requested descriptor for the poller itself.
  if (fd == pollerFd) {
    return false;
  }
  epoll_event event = {};
  if ((interest & kRead) != 0) {
    event.events |= EPOLLIN | EPOLLRDHUP;
  }
  if ((interest & kWrite) != 0) {
    event.events |= EPOLLOUT;
  }
  event.data.fd = fd;
  if (epoll_ctl(pollerFd, EPOLL_CTL_ADD, fd, &event) < 0) {
    if (errno == EBADF) {
      return false;
    }
    throw runtime_error(string("epoll_ctl add failed: ") + strerror(errno));
  }
  return true;
}

}  // namespace et
