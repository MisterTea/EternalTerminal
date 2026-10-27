#include "PollSet.hpp"

namespace et {
set<int> PollSet::waitReadable(int timeoutMs) {
  set<int> ready;
  vector<WSAPOLLFD> pollFds;
  pollFds.reserve(entries.size());
  for (const auto& entry : entries) {
    // CRT stdin is not a socket; WSAPoll would report it invalid every call.
    if (entry.fd == 0) {
      continue;
    }
    WSAPOLLFD pollFd = {};
    pollFd.fd = static_cast<SOCKET>(entry.fd);
    if (entry.read) {
      pollFd.events |= POLLRDNORM;
    }
    if (entry.write) {
      pollFd.events |= POLLWRNORM;
    }
    pollFds.push_back(pollFd);
  }
  if (pollFds.empty()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
    return ready;
  }
  const int pollResult =
      ::WSAPoll(pollFds.data(), static_cast<ULONG>(pollFds.size()), timeoutMs);
  if (pollResult <= 0) {
    return ready;
  }
  for (const auto& pollFd : pollFds) {
    if ((pollFd.events & (POLLRDNORM | POLLRDBAND)) != 0 &&
        (pollFd.revents &
         (POLLRDNORM | POLLRDBAND | POLLERR | POLLHUP | POLLNVAL)) != 0) {
      ready.insert(static_cast<int>(pollFd.fd));
    }
  }
  return ready;
}
}  // namespace et
