#include <atomic>

#include "FakeConsole.hpp"
#include "SshSetupHandler.hpp"
#include "SubprocessUtils.hpp"
#include "TerminalClient.hpp"
#include "TerminalServer.hpp"
#include "TestHeaders.hpp"
#include "TunnelUtils.hpp"

namespace et {

class FakeSubprocessUtils : public SubprocessUtils {
 public:
  virtual string SubprocessToStringInteractive(
      const string& command, const vector<string>& args) override {
    // Generate random id and passkey like SshSetupHandler does
    string id = genRandomAlphaNum(16);
    string passkey = genRandomAlphaNum(32);

    // Return a fake SSH response with the IDPASSKEY format expected by
    // SshSetupHandler
    return "IDPASSKEY:" + id + "/" + passkey;
  }
};

class FakeSshSetupHandler : public SshSetupHandler {
 public:
  explicit FakeSshSetupHandler(shared_ptr<SubprocessUtils> subprocessUtils)
      : SshSetupHandler(subprocessUtils) {}

  pair<string, string> SetupSsh(
      const string& user, const string& host, const string& host_alias,
      int port, const string& jumphost, const string& jServerFifo, bool kill,
      int vlevel, const string& etterminal_path, const string& serverFifo,
      const std::vector<std::string>& ssh_options) override {
    pair<string, string> sshResponse = SshSetupHandler::SetupSsh(
        user, host, host_alias, port, jumphost, jServerFifo, kill, vlevel,
        etterminal_path, serverFifo, ssh_options);

    return sshResponse;
  }
};

}  // namespace et
