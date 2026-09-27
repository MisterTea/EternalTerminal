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
   * @param sshConfigPath An exact SSH configuration path for every SSH
   * process, "none" to disable configuration, or empty for OpenSSH defaults.
   */
  explicit SshSetupHandler(shared_ptr<SubprocessUtils> subprocessUtils,
                           string sshConfigPath = "")
      : subprocessUtils_(subprocessUtils),
        sshConfigPath_(std::move(sshConfigPath)) {}

  /**
   * @brief Constructs the ssh command line for connecting to the ET server.
   */
  virtual pair<string, string> SetupSsh(
      const string& user, const string& host, const string& host_alias,
      int port, const string& jumphost, const string& jServerFifo, bool kill,
      int vlevel, const string& etterminal_path, const string& serverFifo,
      const std::vector<std::string>& ssh_options);

  /** Controls whether non-secret SSH login output is shown to the user. */
  void setDisplayLoginOutput(bool display) { displayLoginOutput_ = display; }

  /**
   * @brief OpenSSH client arguments for the bootstrap ssh. `sshPort` is the
   * sshd port (`-p`), not the etserver port. Pass `sshPortSet` false to leave
   * ssh's own default. Identity files and the cipher spec are forwarded as
   * `-i` and `-c`.
   */
  void setBootstrapOverrides(bool sshPortSet, int sshPort,
                             std::vector<std::string> identityFiles,
                             std::string cipher) {
    sshPortSet_ = sshPortSet;
    sshPort_ = sshPort;
    identityFiles_ = std::move(identityFiles);
    cipher_ = std::move(cipher);
  }

  /** Returns SSH login output with the ET handshake credential removed. */
  static string ExtractLoginOutput(const string& sshOutput);

  /** @brief Path to the packaged `etterminal` helper binary. */
  static const string ETTERMINAL_BIN;

  /**
   * @brief Whether OpenSSH can safely propagate this path through `-J`.
   *
   * Requires a native absolute path. Allowed characters are ASCII letters,
   * digits, `/`, `.`, `_`, and `-`; Windows also allows `:` and `\\` so drive
   * and UNC paths work. Spaces and shell metacharacters are rejected because
   * OpenSSH interpolates `-F` into the implicit ProxyJump command unquoted.
   */
  static bool IsSshConfigPathSafeForProxyJump(const string& path);

 private:
  void appendBootstrapArgs(std::vector<std::string>* sshArgs,
                           bool includeSshPort) const {
    if (includeSshPort && sshPortSet_) {
      sshArgs->push_back("-p");
      sshArgs->push_back(std::to_string(sshPort_));
    }
    for (const auto& identity : identityFiles_) {
      sshArgs->push_back("-i");
      sshArgs->push_back(identity);
    }
    if (!cipher_.empty()) {
      sshArgs->push_back("-c");
      sshArgs->push_back(cipher_);
    }
  }

  shared_ptr<SubprocessUtils> subprocessUtils_;
  string sshConfigPath_;
  bool displayLoginOutput_ = false;
  bool sshPortSet_ = false;
  int sshPort_ = 22;
  std::vector<std::string> identityFiles_;
  std::string cipher_;
};
}  // namespace et
#endif  // __ET_SSH_SETUP_HANDLER__
