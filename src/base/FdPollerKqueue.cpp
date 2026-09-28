#include <sys/event.h>

#include "FdPoller.hpp"

namespace et {
FdPoller::FdPoller() : pollerFd(kqueue()) {
  if (pollerFd < 0) {
    throw runtime_error(string("kqueue failed: ") + strerror(errno));
  }
  if (fcntl(pollerFd, F_SETFD, FD_CLOEXEC) < 0) {
    int savedErrno = errno;
    ::close(pollerFd);
    throw runtime_error(string("fcntl failed: ") + strerror(savedErrno));
  }
}

FdPoller::~FdPoller() { ::close(pollerFd); }

FdPoller::Ready FdPoller::waitImpl(int capacity, int timeoutMs) {
  vector<struct kevent> events(capacity);
  struct timespec timeout = {timeoutMs / 1000,
                             (timeoutMs % 1000) * 1000 * 1000};
  int count = kevent(pollerFd, nullptr, 0, events.data(), capacity, &timeout);
  if (count < 0) {
    if (errno == EINTR) {
      return {};
    }
    throw runtime_error(string("kevent failed: ") + strerror(errno));
  }

  Ready ready;
  for (int i = 0; i < count; ++i) {
    if ((events[i].flags & EV_ERROR) != 0) {
      throw runtime_error(string("kevent reported an error: ") +
                          strerror(static_cast<int>(events[i].data)));
    }
    const int fd = static_cast<int>(events[i].ident);
    if (events[i].filter == EVFILT_READ) {
      ready.readable.insert(fd);
    } else if (events[i].filter == EVFILT_WRITE) {
      ready.writable.insert(fd);
    }
  }
  return ready;
}

// Each direction is a separate filter registration, so each is added and
// deleted on its own.
bool FdPoller::addFd(int fd, short interest) {
  struct kevent changes[2];
  int count = 0;
  if ((interest & kRead) != 0) {
    EV_SET(&changes[count++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0,
           nullptr);
  }
  if ((interest & kWrite) != 0) {
    EV_SET(&changes[count++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0,
           nullptr);
  }
  if (kevent(pollerFd, changes, count, nullptr, 0, nullptr) < 0) {
    if (errno == EBADF) {
      return false;
    }
    throw runtime_error(string("kevent add failed: ") + strerror(errno));
  }
  return true;
}

void FdPoller::removeFd(int fd, short interest) {
  struct kevent changes[2];
  int count = 0;
  if ((interest & kRead) != 0) {
    EV_SET(&changes[count++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  }
  if ((interest & kWrite) != 0) {
    EV_SET(&changes[count++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
  }
  if (kevent(pollerFd, changes, count, nullptr, 0, nullptr) < 0 &&
      errno != EBADF && errno != ENOENT) {
    throw runtime_error(string("kevent delete failed: ") + strerror(errno));
  }
}
}  // namespace et
