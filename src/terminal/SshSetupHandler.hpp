#ifndef __ET_SSH_SETUP_HANDLER__
#define __ET_SSH_SETUP_HANDLER__

#include "Headers.hpp"

namespace et {
/**
 * @brief Responsible for building and launching the helper ssh process.
 */
class SshSetupHandler {
 public:
  /**
   * @brief Constructs the ssh command line for connecting to the ET server.
   */
  static string SetupSsh(const string &user, const string &host,
                         const string &host_alias, int port,
                         const string &jumphost, const string &jServerFifo,
                         bool kill, int vlevel, const string &etterminal_path,
                         const string &serverFifo,
                         const std::vector<std::string> &ssh_options);
  /** @brief Path to the packaged `etterminal` helper binary. */
  static const string ETTERMINAL_BIN;
};
}  // namespace et
#endif  // __ET_SSH_SETUP_HANDLER__
