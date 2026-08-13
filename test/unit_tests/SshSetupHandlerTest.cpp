#include "CryptoHandler.hpp"
#include "SshSetupHandler.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {

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

/**
 * @brief Fake subprocess handler that records every SSH invocation.
 */
class RecordingSshConfigSubprocessHandler : public SubprocessUtils {
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

TEST_CASE("SshSetupHandler with empty SSH output", "[SshSetupHandler]") {
  auto fakeSubprocess = make_shared<FakeSshSubprocessHandlerEmpty>();
  SshSetupHandler handler(fakeSubprocess);

  // This should handle the empty output gracefully
  // The current implementation catches the exception and continues
  auto [id, passkey] = handler.SetupSsh("testuser",  // user
                                        "testhost",  // host
                                        "testhost",  // host_alias
                                        2022,        // port
                                        "",          // jumphost
                                        "",          // jServerFifo
                                        false,       // kill
                                        0,           // vlevel
                                        "",          // etterminal_path
                                        "",          // serverFifo
                                        std::vector<string>()  // ssh_options
  );

  SECTION("Returns default id/passkey on failure") {
    // When ssh fails, the handler should still return some id/passkey
    // The current implementation initializes id and passkey to empty strings
    // and they remain empty if the SSH call fails
    REQUIRE(id.length() == 16);
    REQUIRE(passkey.length() == 32);
  }
}

TEST_CASE("SshSetupHandler with invalid server output", "[SshSetupHandler]") {
  auto fakeSubprocess = make_shared<FakeSshSubprocessHandlerInvalid>();
  SshSetupHandler handler(fakeSubprocess);

  auto [id, passkey] = handler.SetupSsh("testuser",  // user
                                        "testhost",  // host
                                        "testhost",  // host_alias
                                        2022,        // port
                                        "",          // jumphost
                                        "",          // jServerFifo
                                        false,       // kill
                                        0,           // vlevel
                                        "",          // etterminal_path
                                        "",          // serverFifo
                                        std::vector<string>()  // ssh_options
  );

  SECTION("Handles missing IDPASSKEY gracefully") {
    // When the server output doesn't contain IDPASSKEY,
    // the handler catches the exception and continues
    REQUIRE(id.length() == 16);
    REQUIRE(passkey.length() == 32);
  }
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
  auto fakeSubprocess = make_shared<RecordingSshConfigSubprocessHandler>();
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
  auto fakeSubprocess = make_shared<RecordingSshConfigSubprocessHandler>();
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
  REQUIRE(SshSetupHandler::IsSshConfigPathSafeForProxyJump(
      "/private/et-client_1/ssh.config"));

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
