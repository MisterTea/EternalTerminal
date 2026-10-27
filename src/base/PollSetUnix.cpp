#include "PollSet.hpp"

namespace et {
set<int> PollSet::waitReadable(int timeoutMs) {
  set<int> ready;
  if (entries.empty()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
    return ready;
  }
  vector<struct pollfd> pollFds;
  pollFds.reserve(entries.size());
  for (const auto& entry : entries) {
    short events = 0;
    if (entry.read) {
      events |= POLLIN;
    }
    if (entry.write) {
      events |= POLLOUT;
    }
    pollFds.push_back({entry.fd, events, 0});
  }
  const int pollResult =
      ::poll(pollFds.data(), static_cast<nfds_t>(pollFds.size()), timeoutMs);
  if (pollResult < 0 && errno != EINTR) {
    FATAL_FAIL(pollResult);
  }
  for (const auto& pollFd : pollFds) {
    if ((pollFd.events & POLLIN) != 0 &&
        (pollFd.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0) {
      ready.insert(pollFd.fd);
    }
  }
  return ready;
}
}  // namespace et
