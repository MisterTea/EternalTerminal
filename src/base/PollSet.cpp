#include "PollSet.hpp"

namespace et {
void PollSet::watch(int fd, bool read, bool write) {
  if (fd < 0) {
    return;
  }
  for (auto& entry : entries) {
    if (entry.fd == fd) {
      entry.read = entry.read || read;
      entry.write = entry.write || write;
      return;
    }
  }
  entries.push_back({fd, read, write});
}
}  // namespace et
