#include "CryptoHandler.hpp"
#include "SshSetupHandler.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {

const string kTestId = "0123456789ABCDEF";
const string kTestPasskey = "0123456789ABCDEF0123456789ABCDEF";

pair<string, string> bootstrapCredentialsFromArgs(const vector<string>& args) {
  if (args.empty()) {
    return {};
  }

  const string& command = args.back();
  const string echoPrefix = "echo '";
  const size_t credentialsStart = command.find(echoPrefix);
  if (credentialsStart == string::npos) {
    return {};
  }

  const size_t idStart = credentialsStart + echoPrefix.length();
  const size_t separator = command.find('/', idStart);
  const size_t passkeyEnd = command.find('_', separator);
  if (separator == string::npos || passkeyEnd == string::npos ||
      separator <= idStart || passkeyEnd <= separator + 1) {
    return {};
  }

  return {command.substr(idStart, separator - idStart),
          command.substr(separator + 1, passkeyEnd - separator - 1)};
}

/** Captures dispatched log messages for assertions about secret exposure. */
class SshSetupLogCapture : public el::LogDispatchCallback {
 public:
  void handle(const el::LogDispatchData* data) override {
    messages_ += data->logMessage()->message();
    messages_ += '\n';
  }

  const string& messages() const { return messages_; }

 private:
  string messages_;
};

class ScopedSshSetupLogCapture {
 public:
  ScopedSshSetupLogCapture() {
    el::Helpers::installLogDispatchCallback<SshSetupLogCapture>(callbackId());
    callback_ =
        el::Helpers::logDispatchCallback<SshSetupLogCapture>(callbackId());
  }

  ~ScopedSshSetupLogCapture() {
    el::Helpers::uninstallLogDispatchCallback<SshSetupLogCapture>(callbackId());
  }

  const string& messages() const { return callback_->messages(); }

 private:
  static const string& callbackId() {
    static const string id = "SshSetupHandlerTestLogCapture";
    return id;
  }

  SshSetupLogCapture* callback_ = nullptr;
};

class ScopedVerboseLogging {
 public:
  ScopedVerboseLogging() : previous_(el::Loggers::verboseLevel()) {
    el::Loggers::setVerboseLevel(1);
  }

  ~ScopedVerboseLogging() { el::Loggers::setVerboseLevel(previous_); }

 private:
  decltype(el::Loggers::verboseLevel()) previous_;
};

/**
 * @brief Fake subprocess handler that simulates ssh command execution
 * for testing SshSetupHandler.
 */
class FakeSshSubprocessHandler : public SubprocessUtils {
 public:
  /**
   * @brief Simulates the subprocess execution for ssh commands.
   * Verifies that the command is "ssh" and returns a simulated server response
   * similar to what etterminal would output (TerminalMain.cpp lines 117-119).
   */
  string SubprocessToStringInteractive(const string& command,
                                       const vector<string>& args) override {
    // Verify the command is "ssh"
    REQUIRE(command == "ssh");

    // Simulate the server response
    // When etterminal receives an id starting with "XXX", it generates
    // new id and passkey and outputs them in IDPASSKEY format
    string id = genRandomAlphaNum(16);
    string passkey = genRandomAlphaNum(32);
    string idpasskey = id + string("/") + passkey;

    return string("IDPASSKEY:") + idpasskey;
  }
};

class FakeSshSubprocessHandlerWithMotd : public SubprocessUtils {
 public:
  string SubprocessToStringInteractive(const string& command,
                                       const vector<string>& args) override {
    REQUIRE(command == "ssh");
    return "Welcome to the test server\nIDPASSKEY:" + string(16, 'i') + "/" +
           string(32, 'p') + "\n";
  }
};

/**
 * @brief Fake subprocess handler that returns empty output
 * to simulate SSH connection failure.
 */
class FakeSshSubprocessHandlerEmpty : public SubprocessUtils {
 public:
  string SubprocessToStringInteractive(const string& command,
                                       const vector<string>& args) override {
    REQUIRE(command == "ssh");
    return "";
  }
};

/**
 * @brief Fake subprocess handler that returns invalid output
 * to simulate server misconfiguration.
 */
class FakeSshSubprocessHandlerInvalid : public SubprocessUtils {
 public:
  string SubprocessToStringInteractive(const string& command,
                                       const vector<string>& args) override {
    REQUIRE(command == "ssh");
    return "Some invalid output without IDPASSKEY";
  }
};

/** Fake handler with a caller-supplied response for malformed-output tests. */
class FakeSshSubprocessHandlerWithResponse : public SubprocessUtils {
 public:
  explicit FakeSshSubprocessHandlerWithResponse(const string& response)
      : responses_{response} {}

  explicit FakeSshSubprocessHandlerWithResponse(const vector<string>& responses)
      : responses_(responses) {}

  string SubprocessToStringInteractive(const string& command,
                                       const vector<string>& args) override {
    REQUIRE(command == "ssh");
    REQUIRE(!responses_.empty());
    lastArgs_ = args;
    const size_t responseIndex = min(callCount_, responses_.size() - 1);
    ++callCount_;
    return responses_[responseIndex];
  }

  const vector<string>& lastArgs() const { return lastArgs_; }

 private:
  vector<string> responses_;
  vector<string> lastArgs_;
  size_t callCount_ = 0;
};

/** Returns the bootstrap pair, emulating an old server that reuses it. */
class FakeSshSubprocessHandlerLegacy : public SubprocessUtils {
 public:
  string SubprocessToStringInteractive(const string& command,
                                       const vector<string>& args) override {
    REQUIRE(command == "ssh");
    bootstrap_ = bootstrapCredentialsFromArgs(args);
    REQUIRE(bootstrap_.first.length() == 16);
    REQUIRE(bootstrap_.second.length() == 32);
    return "IDPASSKEY:" + bootstrap_.first + "/" + bootstrap_.second;
  }

  const pair<string, string>& bootstrap() const { return bootstrap_; }

 private:
  pair<string, string> bootstrap_;
};

class FakeSshSubprocessHandlerThrows : public SubprocessUtils {
 public:
  explicit FakeSshSubprocessHandlerThrows(const string& secret)
      : secret_(secret) {}

  string SubprocessToStringInteractive(const string& command,
                                       const vector<string>& args) override {
    REQUIRE(command == "ssh");
    throw runtime_error("ssh failed while using " + secret_);
  }

 private:
  string secret_;
};

/**
 * @brief Fake subprocess handler that simulates jumphost setup.
 */
class FakeSshSubprocessHandlerWithJumphost : public SubprocessUtils {
 public:
  string SubprocessToStringInteractive(const string& command,
                                       const vector<string>& args) override {
    REQUIRE(command == "ssh");

    // Generate id and passkey
    string id = genRandomAlphaNum(16);
    string passkey = genRandomAlphaNum(32);
    string idpasskey = id + string("/") + passkey;

    // Check if this is the jumphost call (args.size() == 2)
    // or the initial ssh call (args.size() > 2)
    if (args.size() == 2) {
      // This is the jumphost call
      // Return format similar to what the jumpclient would output
      return string("IDPASSKEY:") + idpasskey;
    } else {
      // This is the initial ssh call
      return string("IDPASSKEY:") + idpasskey;
    }
  }
};

/**
 * @brief Fake subprocess handler that records every SSH invocation.
 */
class RecordingSshSubprocessHandler : public SubprocessUtils {
 public:
  vector<vector<string>> calls;

  string SubprocessToStringInteractive(const string& command,
                                       const vector<string>& args) override {
    REQUIRE(command == "ssh");
    calls.push_back(args);

    string id = genRandomAlphaNum(16);
    string passkey = genRandomAlphaNum(32);
    return string("IDPASSKEY:") + id + "/" + passkey;
  }
};

}  // namespace

TEST_CASE("SshSetupHandler basic connection", "[SshSetupHandler]") {
  auto fakeSubprocess = make_shared<FakeSshSubprocessHandler>();
  SshSetupHandler handler(fakeSubprocess);

  auto [id, passkey] = handler.SetupSsh("testuser",  // user
                                        "testhost",  // host
                                        "testhost",  // host_alias
                                        2022,        // port
                                        "",          // jumphost (empty)
                                        "",          // jServerFifo
                                        false,       // kill
                                        0,           // vlevel
                                        "",          // etterminal_path
                                        "",          // serverFifo
                                        std::vector<string>()  // ssh_options
  );

  SECTION("Returns id/passkey pair") {
    // Verify id and passkey have expected lengths
    REQUIRE(id.length() == 16);
    REQUIRE(passkey.length() == 32);
  }
}

TEST_CASE("SshSetupHandler removes credentials from login output",
          "[SshSetupHandler]") {
  const string credential = string(16, 'i') + "/" + string(32, 'p');

  REQUIRE(SshSetupHandler::ExtractLoginOutput(
              "Welcome to the server\nIDPASSKEY:" + credential + "\n") ==
          "Welcome to the server\n");
  REQUIRE(SshSetupHandler::ExtractLoginOutput("IDPASSKEY:" + credential +
                                              "\r\n") == "");
  REQUIRE(SshSetupHandler::ExtractLoginOutput("plain login output") ==
          "plain login output");
}

TEST_CASE("SshSetupHandler displays login output only when enabled",
          "[SshSetupHandler]") {
  auto runSetup = [](bool displayLoginOutput) {
    auto fakeSubprocess = make_shared<FakeSshSubprocessHandlerWithMotd>();
    SshSetupHandler handler(fakeSubprocess);
    handler.setDisplayLoginOutput(displayLoginOutput);

    std::ostringstream output;
    auto* previousBuffer = std::cout.rdbuf(output.rdbuf());
    handler.SetupSsh("testuser", "testhost", "testhost", 2022, "", "", false, 0,
                     "", "", {});
    el::Loggers::flushAll();
    std::cout.rdbuf(previousBuffer);
    return output.str();
  };

  const string interactiveOutput = runSetup(true);
  REQUIRE(interactiveOutput.find("Welcome to the test server") != string::npos);
  REQUIRE(interactiveOutput.find("IDPASSKEY:") == string::npos);
  REQUIRE(interactiveOutput.find(string(16, 'i') + "/" + string(32, 'p')) ==
          string::npos);

  REQUIRE(runSetup(false).find("Welcome to the test server") == string::npos);
}

TEST_CASE("SshSetupHandler with custom options", "[SshSetupHandler]") {
  auto fakeSubprocess = make_shared<FakeSshSubprocessHandler>();
  SshSetupHandler handler(fakeSubprocess);

  std::vector<string> ssh_options = {"StrictHostKeyChecking=no",
                                     "UserKnownHostsFile=/dev/null"};

  auto [id, passkey] = handler.SetupSsh("customuser",  // user
                                        "customhost",  // host
                                        "customhost",  // host_alias
                                        2023,          // port
                                        "",            // jumphost
                                        "",            // jServerFifo
                                        true,  // kill (kill old sessions)
                                        2,     // vlevel (verbose level)
                                        "/custom/path",  // etterminal_path
                                        "/tmp/fifo",     // serverFifo
                                        ssh_options      // ssh_options
  );

  // Verify result is valid
  REQUIRE(id.length() == 16);
  REQUIRE(passkey.length() == 32);
}

TEST_CASE("SshSetupHandler with jumphost", "[SshSetupHandler]") {
  auto fakeSubprocess = make_shared<FakeSshSubprocessHandlerWithJumphost>();
  SshSetupHandler handler(fakeSubprocess);

  auto [id, passkey] = handler.SetupSsh("testuser",  // user
                                        "testhost",  // host
                                        "testhost",  // host_alias
                                        2022,        // port
                                        "jumphost",  // jumphost (non-empty)
                                        "",          // jServerFifo
                                        false,       // kill
                                        0,           // vlevel
                                        "",          // etterminal_path
                                        "",          // serverFifo
                                        std::vector<string>()  // ssh_options
  );

  SECTION("Returns id/passkey pair with jumphost") {
    REQUIRE(id.length() == 16);
    REQUIRE(passkey.length() == 32);
  }
}

TEST_CASE("SshSetupHandler keeps destination options off the jumphost",
          "[SshSetupHandler]") {
  auto fakeSubprocess = make_shared<RecordingSshSubprocessHandler>();
  SshSetupHandler handler(fakeSubprocess);
  const vector<string> destination_options = {
      "User=target-user",
      "HostKeyAlias=target",
      "UserKnownHostsFile=/tmp/target_known_hosts",
      "HostKeyAlgorithms=ssh-ed25519-cert-v01@openssh.com",
      "KbdInteractiveAuthentication=no",
  };

  auto [id, passkey] = handler.SetupSsh(
      "target-user", "target.internal", "target", 2022,
      "jump-user@jump.example:2222", "", false, 0, "", "", destination_options);

  REQUIRE(id.length() == 16);
  REQUIRE(passkey.length() == 32);
  REQUIRE(fakeSubprocess->calls.size() == 2);

  const auto& destination_args = fakeSubprocess->calls[0];
  REQUIRE(destination_args[0] == "-J");
  REQUIRE(destination_args[1] == "jump-user@jump.example:2222");
  REQUIRE(destination_args[2] == "target-user@target");
  for (const auto& option : destination_options) {
    REQUIRE(std::find(destination_args.begin(), destination_args.end(),
                      "-o" + option) != destination_args.end());
  }

  const auto& jump_args = fakeSubprocess->calls[1];
  REQUIRE(jump_args.size() == 4);
  REQUIRE(jump_args[0] == "-p");
  REQUIRE(jump_args[1] == "2222");
  REQUIRE(jump_args[2] == "jump-user@jump.example");
  REQUIRE(
      jump_args[3].find("--jump --dsthost=target.internal --dstport=2022") !=
      string::npos);
  for (const auto& option : destination_options) {
    REQUIRE(std::find(jump_args.begin(), jump_args.end(), "-o" + option) ==
            jump_args.end());
  }
}

TEST_CASE("SshSetupHandler with empty SSH output", "[SshSetupHandler]") {
  auto fakeSubprocess = make_shared<FakeSshSubprocessHandlerEmpty>();
  SshSetupHandler handler(fakeSubprocess);

  REQUIRE_THROWS_AS(handler.SetupSsh("testuser",            // user
                                     "testhost",            // host
                                     "testhost",            // host_alias
                                     2022,                  // port
                                     "",                    // jumphost
                                     "",                    // jServerFifo
                                     false,                 // kill
                                     0,                     // vlevel
                                     "",                    // etterminal_path
                                     "",                    // serverFifo
                                     std::vector<string>()  // ssh_options
                                     ),
                    std::runtime_error);
}

TEST_CASE("SshSetupHandler with invalid server output", "[SshSetupHandler]") {
  auto fakeSubprocess = make_shared<FakeSshSubprocessHandlerInvalid>();
  SshSetupHandler handler(fakeSubprocess);

  REQUIRE_THROWS_AS(handler.SetupSsh("testuser",            // user
                                     "testhost",            // host
                                     "testhost",            // host_alias
                                     2022,                  // port
                                     "",                    // jumphost
                                     "",                    // jServerFifo
                                     false,                 // kill
                                     0,                     // vlevel
                                     "",                    // etterminal_path
                                     "",                    // serverFifo
                                     std::vector<string>()  // ssh_options
                                     ),
                    std::runtime_error);
}

TEST_CASE("SshSetupHandler accepts old and new server handshakes",
          "[SshSetupHandler]") {
  auto legacySubprocess = make_shared<FakeSshSubprocessHandlerLegacy>();
  SshSetupHandler legacyHandler(legacySubprocess);

  auto [legacyId, legacyPasskey] =
      legacyHandler.SetupSsh("testuser", "testhost", "testhost", 2022, "", "",
                             false, 0, "", "", std::vector<string>());

  REQUIRE(legacyId == legacySubprocess->bootstrap().first);
  REQUIRE(legacyPasskey == legacySubprocess->bootstrap().second);

  auto newSubprocess = make_shared<FakeSshSubprocessHandlerWithResponse>(
      "login noise\nIDPASSKEY:" + kTestId + "/" + kTestPasskey + "\n");
  SshSetupHandler newHandler(newSubprocess);

  auto [newId, newPasskey] =
      newHandler.SetupSsh("testuser", "testhost", "testhost", 2022, "", "",
                          false, 0, "", "", std::vector<string>());

  REQUIRE(newId == kTestId);
  REQUIRE(newPasskey == kTestPasskey);
}

TEST_CASE("SshSetupHandler does not log bootstrap credentials",
          "[SshSetupHandler]") {
  auto fakeSubprocess = make_shared<FakeSshSubprocessHandlerLegacy>();
  SshSetupHandler handler(fakeSubprocess);
  ScopedSshSetupLogCapture logCapture;
  ScopedVerboseLogging verboseLogging;

  REQUIRE_NOTHROW(handler.SetupSsh("testuser", "testhost", "testhost", 2022, "",
                                   "", false, 0, "", "",
                                   std::vector<string>()));
  REQUIRE(logCapture.messages().find(fakeSubprocess->bootstrap().first) ==
          string::npos);
  REQUIRE(logCapture.messages().find(fakeSubprocess->bootstrap().second) ==
          string::npos);
}

TEST_CASE("SshSetupHandler rejects malformed direct output without leaking it",
          "[SshSetupHandler]") {
  const string malformedOutput = "IDPASSKEY:" + kTestId;
  auto fakeSubprocess =
      make_shared<FakeSshSubprocessHandlerWithResponse>(malformedOutput);
  SshSetupHandler handler(fakeSubprocess);
  ScopedSshSetupLogCapture logCapture;
  ScopedVerboseLogging verboseLogging;

  REQUIRE_THROWS_AS(
      handler.SetupSsh("testuser", "testhost", "testhost", 2022, "", "", false,
                       0, "", "", std::vector<string>()),
      std::runtime_error);
  REQUIRE(logCapture.messages().find(kTestId) == string::npos);
  REQUIRE(logCapture.messages().find(malformedOutput) == string::npos);
}

TEST_CASE("SshSetupHandler hides subprocess errors", "[SshSetupHandler]") {
  const string secret = kTestId + "/" + kTestPasskey;
  auto fakeSubprocess = make_shared<FakeSshSubprocessHandlerThrows>(secret);
  SshSetupHandler handler(fakeSubprocess);
  ScopedSshSetupLogCapture logCapture;

  REQUIRE_THROWS_AS(
      handler.SetupSsh("testuser", "testhost", "testhost", 2022, "", "", false,
                       0, "", "", std::vector<string>()),
      std::runtime_error);
  REQUIRE(logCapture.messages().find(secret) == string::npos);
}

TEST_CASE("SshSetupHandler rejects malformed jump output without fallback",
          "[SshSetupHandler]") {
  const string malformedJumpOutput = "IDPASSKEY:" + kTestId;
  auto fakeSubprocess =
      make_shared<FakeSshSubprocessHandlerWithResponse>(vector<string>{
          "IDPASSKEY:" + kTestId + "/" + kTestPasskey, malformedJumpOutput});
  SshSetupHandler handler(fakeSubprocess);
  ScopedSshSetupLogCapture logCapture;

  REQUIRE_THROWS_AS(
      handler.SetupSsh("testuser", "testhost", "testhost", 2022, "jumphost", "",
                       false, 0, "", "", std::vector<string>()),
      std::runtime_error);
  REQUIRE(logCapture.messages().find(kTestId) == string::npos);
  REQUIRE(logCapture.messages().find(malformedJumpOutput) == string::npos);
}

TEST_CASE("SshSetupHandler with serverFifo", "[SshSetupHandler]") {
  auto fakeSubprocess = make_shared<FakeSshSubprocessHandler>();
  SshSetupHandler handler(fakeSubprocess);

  auto [id, passkey] = handler.SetupSsh("testuser",          // user
                                        "testhost",          // host
                                        "testhost",          // host_alias
                                        2022,                // port
                                        "",                  // jumphost
                                        "",                  // jServerFifo
                                        false,               // kill
                                        1,                   // vlevel
                                        "",                  // etterminal_path
                                        "/tmp/server.fifo",  // serverFifo
                                        std::vector<string>()  // ssh_options
  );

  REQUIRE(id.length() == 16);
  REQUIRE(passkey.length() == 32);
}

TEST_CASE("SshSetupHandler with jumphost and jServerFifo",
          "[SshSetupHandler]") {
  auto fakeSubprocess = make_shared<FakeSshSubprocessHandlerWithJumphost>();
  SshSetupHandler handler(fakeSubprocess);

  auto [id, passkey] = handler.SetupSsh("testuser",        // user
                                        "testhost",        // host
                                        "testhost",        // host_alias
                                        2022,              // port
                                        "jumphost",        // jumphost
                                        "/tmp/jump.fifo",  // jServerFifo
                                        false,             // kill
                                        0,                 // vlevel
                                        "",                // etterminal_path
                                        "",                // serverFifo
                                        std::vector<string>()  // ssh_options
  );

  REQUIRE(id.length() == 16);
  REQUIRE(passkey.length() == 32);
}

TEST_CASE("SshSetupHandler can select one exact SSH configuration",
          "[SshSetupHandler]") {
  auto fakeSubprocess = make_shared<RecordingSshSubprocessHandler>();
  const string config_path = "/private/et-client/ssh_config";
  SshSetupHandler handler(fakeSubprocess, config_path);

  auto [id, passkey] = handler.SetupSsh(
      "exact-target-user", "exact-target.example", "exact-target.example", 2022,
      "exact-jump-user@exact-jump.example:2222", "", false, 0, "", "", {});

  REQUIRE(id.length() == 16);
  REQUIRE(passkey.length() == 32);
  REQUIRE(fakeSubprocess->calls.size() == 2);

  const auto& destination_args = fakeSubprocess->calls[0];
  REQUIRE(destination_args.size() == 6);
  REQUIRE(destination_args[0] == "-F");
  REQUIRE(destination_args[1] == config_path);
  REQUIRE(destination_args[2] == "-J");
  REQUIRE(destination_args[3] == "exact-jump-user@exact-jump.example:2222");
  REQUIRE(destination_args[4] == "exact-target-user@exact-target.example");

  const auto& jump_args = fakeSubprocess->calls[1];
  REQUIRE(jump_args.size() == 6);
  REQUIRE(jump_args[0] == "-F");
  REQUIRE(jump_args[1] == config_path);
  REQUIRE(jump_args[2] == "-p");
  REQUIRE(jump_args[3] == "2222");
  REQUIRE(jump_args[4] == "exact-jump-user@exact-jump.example");
  REQUIRE(jump_args[5].find(
              "--jump --dsthost=exact-target.example --dstport=2022") !=
          string::npos);
}

TEST_CASE("SshSetupHandler can disable all SSH configuration",
          "[SshSetupHandler]") {
  auto fakeSubprocess = make_shared<RecordingSshSubprocessHandler>();
  SshSetupHandler handler(fakeSubprocess, "none");

  handler.SetupSsh("exact-user", "exact-target.example", "exact-target.example",
                   2022, "exact-jump-user@exact-jump.example", "", false, 0, "",
                   "", {});

  REQUIRE(fakeSubprocess->calls.size() == 2);
  const auto& destination_args = fakeSubprocess->calls[0];
  REQUIRE(destination_args[0] == "-F");
  REQUIRE(destination_args[1] == "none");
  REQUIRE(destination_args[2] == "-J");
  REQUIRE(destination_args[3] == "exact-jump-user@exact-jump.example");
  REQUIRE(destination_args[4] == "exact-user@exact-target.example");

  const auto& jump_args = fakeSubprocess->calls[1];
  REQUIRE(jump_args[0] == "-F");
  REQUIRE(jump_args[1] == "none");
  REQUIRE(jump_args[2] == "exact-jump-user@exact-jump.example");
}

TEST_CASE("SSH config paths are safe for OpenSSH ProxyJump",
          "[SshSetupHandler]") {
#ifdef WIN32
  REQUIRE(SshSetupHandler::IsSshConfigPathSafeForProxyJump(
      "C:\\et-client_1\\ssh.config"));
  REQUIRE(SshSetupHandler::IsSshConfigPathSafeForProxyJump(
      "C:/et-client_1/ssh.config"));
  REQUIRE(SshSetupHandler::IsSshConfigPathSafeForProxyJump(
      "\\\\server\\share\\ssh_config"));
  REQUIRE_FALSE(SshSetupHandler::IsSshConfigPathSafeForProxyJump(
      "/private/et-client_1/ssh.config"));
  REQUIRE_FALSE(
      SshSetupHandler::IsSshConfigPathSafeForProxyJump("C:et\\ssh.config"));
  REQUIRE_FALSE(
      SshSetupHandler::IsSshConfigPathSafeForProxyJump("\\et\\ssh.config"));
  REQUIRE_FALSE(SshSetupHandler::IsSshConfigPathSafeForProxyJump(
      "C:\\Users\\foo bar\\config"));
  REQUIRE_FALSE(
      SshSetupHandler::IsSshConfigPathSafeForProxyJump("C:\\et\\config;cmd"));
#else
  REQUIRE(SshSetupHandler::IsSshConfigPathSafeForProxyJump(
      "/private/et-client_1/ssh.config"));
  REQUIRE_FALSE(SshSetupHandler::IsSshConfigPathSafeForProxyJump(
      "C:\\et-client_1\\ssh.config"));
#endif

  const vector<string> unsafe_paths = {
      "relative/config",        "/private/config with-space",
      "/private/config\\path",  "/private/config$variable",
      "/private/config`cmd`",   "/private/config%token",
      "/private/config;cmd",    "/private/config&cmd",
      "/private/config|cmd",    "/private/config(cmd)",
      "/private/config*glob",   "/private/config?glob",
      "/private/config[glob]",  "/private/config'quote",
      "/private/config\"quote", "",
  };
  for (const auto& path : unsafe_paths) {
    REQUIRE_FALSE(SshSetupHandler::IsSshConfigPathSafeForProxyJump(path));
  }
}
