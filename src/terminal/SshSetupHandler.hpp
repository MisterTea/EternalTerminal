#ifndef __ET_SSH_SETUP_HANDLER__
#define __ET_SSH_SETUP_HANDLER__

#include "Headers.hpp"
#include "SubprocessUtils.hpp"

namespace et {
/**
 * @brief Responsible for building and launching the helper ssh process.
 */
class SshSetupHandler {
 public:
  /**
   * @brief Constructs an SshSetupHandler with a subprocess utility.
   * @param subprocessUtils The subprocess utility to use for running ssh.
   */
  explicit SshSetupHandler(shared_ptr<SubprocessUtils> subprocessUtils)
      : subprocessUtils_(subprocessUtils) {}

  /**
   * @brief Constructs the ssh command line for connecting to the ET server.
   */
  virtual pair<string, string> SetupSsh(
      const string& user, const string& host, const string& host_alias,
      int port, const string& jumphost, const string& jServerFifo, bool kill,
      int vlevel, const string& etterminal_path, const string& serverFifo,
      const std::vector<std::string>& ssh_options);

  /** @brief Path to the packaged `etterminal` helper binary. */
  static const string ETTERMINAL_BIN;

 private:
  shared_ptr<SubprocessUtils> subprocessUtils_;
};
}  // namespace et
#endif  // __ET_SSH_SETUP_HANDLER__
