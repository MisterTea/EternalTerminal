#include "HtmServer.hpp"

namespace et {
int64_t HtmServer::controlClientPid(int /*fd*/) { return -1; }

// The Windows htm client exits on its own once the endpoint closes.
void HtmServer::reapControlClient(int64_t /*peer*/) {}

string HtmServer::getPipeName() {
  return string("htm.") + GetHtmIpcUser() + string(".ipc");
}
}  // namespace et
