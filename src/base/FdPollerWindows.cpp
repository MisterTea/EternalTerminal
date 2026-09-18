#ifdef WIN32
#include "FdPoller.hpp"

namespace et {
FdPoller::FdPoller() : pollerFd(-1) {}

FdPoller::~FdPoller() {}

FdPoller::Ready FdPoller::waitImpl(int /*capacity*/, int timeoutMs) {
  Ready ready;
  if (registeredFds.empty()) {
    // No fds watched: sleep for the timeout so callers keep their cadence.
    if (timeoutMs > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
    }
    return ready;
  }

  // WSAPoll rather than select(): like epoll/kqueue it has no FD_SETSIZE
  // ceiling, so a descriptor at or above 1024 is safe to watch.
  vector<WSAPOLLFD> pollFds;
  pollFds.reserve(registeredFds.size());
  vector<int> fds;
  fds.reserve(registeredFds.size());
  for (const auto& [fd, interest] : registeredFds) {
    WSAPOLLFD pollFd = {};
    pollFd.fd = static_cast<SOCKET>(fd);
    if ((interest & kRead) != 0) {
      pollFd.events |= POLLRDNORM | POLLRDBAND;
    }
    if ((interest & kWrite) != 0) {
      pollFd.events |= POLLWRNORM;
    }
    pollFds.push_back(pollFd);
    fds.push_back(fd);
  }

  const int rc = ::WSAPoll(pollFds.data(), static_cast<ULONG>(pollFds.size()),
                           timeoutMs < 0 ? INFINITE : timeoutMs);
  if (rc < 0) {
    const int err = WSAGetLastError();
    if (err == WSAEINTR) {
      return {};
    }
    if (err == WSAENOTSOCK || err == WSAEINVAL) {
      // A socket closed under us invalidates the whole set for WSAPoll.
      // Report nothing this round; the next setFds() drops the dead
      // descriptor (as the epoll backend does via EBADF) and polling resumes.
      return {};
    }
    throw std::runtime_error(string("WSAPoll failed: ") + to_string(err));
  }
  if (rc == 0) {
    return ready;
  }
  for (size_t i = 0; i < pollFds.size(); ++i) {
    const short revents = pollFds[i].revents;
    if (revents == 0) {
      continue;
    }
    const short interest = registeredFds[fds[i]];
    // HUP/ERR arrive unrequested, so report them only in the direction the
    // caller asked about, mirroring the epoll backend. An invalid socket
    // (POLLNVAL, e.g. closed under us) surfaces in every interested direction
    // so the caller attempts I/O and observes the failure, mirroring poll().
    if ((revents & POLLNVAL) != 0) {
      if ((interest & kRead) != 0) {
        ready.readable.insert(fds[i]);
      }
      if ((interest & kWrite) != 0) {
        ready.writable.insert(fds[i]);
      }
      continue;
    }
    if ((interest & kRead) != 0 &&
        (revents & (POLLRDNORM | POLLRDBAND | POLLHUP | POLLERR)) != 0) {
      ready.readable.insert(fds[i]);
    }
    if ((interest & kWrite) != 0 &&
        (revents & (POLLWRNORM | POLLWRBAND | POLLHUP | POLLERR)) != 0) {
      ready.writable.insert(fds[i]);
    }
  }
  return ready;
}

bool FdPoller::addFd(int fd, short /*interest*/) {
  if (fd < 0) {
    return false;
  }
  // WSAPoll validates lazily at wait() time; accept everything here and let
  // waitImpl surface (and tolerate) closed sockets.
  return true;
}

void FdPoller::removeFd(int /*fd*/, short /*interest*/) {}

}  // namespace et
#endif
