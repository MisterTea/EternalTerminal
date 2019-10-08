#ifndef __ET_SSH_SETUP_HANDLER__
#define __ET_SSH_SETUP_HANDLER__

#include "Headers.hpp"

namespace et {
class SshSetupHandler {
 public:
  static string SetupSsh(const string &user, const string &host,
                         const string &host_alias, int port,
                         const string &jumphost, int jport, bool kill,
                         int vlevel, const string &cmd_prefix,
                         const string &serverFifo);
};
}  // namespace et
#endif  // __ET_SSH_SETUP_HANDLER__
