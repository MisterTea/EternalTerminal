#ifndef __ETERNAL_TCP_SSH_SETUP_HANDLER__
#define __ETERNAL_TCP_SSH_SETUP_HANDLER__

#include "Headers.hpp"

namespace et {
class SshSetupHandler {
 public:
  static string SetupSsh(string user, string host, int port, string jumphost,
                         int jport);
};
}  // namespace et
#endif  // __ETERNAL_TCP_SSH_SETUP_HANDLER__
