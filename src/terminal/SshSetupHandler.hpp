#ifndef __ET_SSH_SETUP_HANDLER__
#define __ET_SSH_SETUP_HANDLER__

#include "Headers.hpp"

namespace et {
class SshSetupHandler {
 public:
  static string SetupSsh(string user, string host, string host_alias, int port,
                         string jumphost, int jport, bool kill, int vlevel,
                         string cmd_prefix, bool noratelimit);
};
}  // namespace et
#endif  // __ET_SSH_SETUP_HANDLER__
