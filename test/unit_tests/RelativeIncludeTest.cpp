#include "OpenSshLocalQueries.hpp"
#include "ParseConfigFile.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("Relative Include resolves against including file", "[SSHConfig]") {
  REQUIRE(resolveIncludePath("hosts/work", "/home/user/.ssh") ==
          "/home/user/.ssh/hosts/work");
  REQUIRE(resolveIncludePath("../shared", "/home/user/.ssh/conf.d") ==
          "/home/user/.ssh/shared");
  REQUIRE(resolveIncludePath("/etc/ssh/common", "/home/user/.ssh") ==
          "/etc/ssh/common");
}

TEST_CASE("Relative Include parses nested configs and skips missing wildcards",
          "[SSHConfig]") {
  const fs::path tempDir = fs::temp_directory_path() /
                           ("et_test_ssh_include_" + sole::uuid4().str());
  fs::remove_all(tempDir);
  fs::create_directories(tempDir / "conf.d");

  const fs::path mainConfig = tempDir / "config";
  const fs::path nestedConfig = tempDir / "conf.d" / "work";

  std::ofstream(mainConfig)
      << "Include conf.d/work\nInclude nonexistent_dir/*\n";
  std::ofstream(nestedConfig)
      << "Host testhost\n    HostName 192.168.1.100\n    User testuser\n    "
         "Port 2222\n";

  Options opts = {NULL, NULL, NULL, NULL, NULL, NULL, 0,    0, 0,
                  0,    0,    NULL, NULL, 0,    0,    NULL, {}};
  ssh_options_set(&opts, SSH_OPTIONS_HOST, "testhost");
  int rc = parse_ssh_config_file("testhost", &opts, mainConfig.string());
  REQUIRE(rc == 0);
  REQUIRE(opts.host != nullptr);
  REQUIRE(string(opts.host) == "192.168.1.100");
  REQUIRE(opts.username != nullptr);
  REQUIRE(string(opts.username) == "testuser");
  REQUIRE(opts.port == 2222);

  freeOptionsFields(&opts);
  fs::remove_all(tempDir);
}

TEST_CASE("OpenSSH -o session options update resolved config",
          "[OpenSshLocalQueries]") {
  Options opts = {};
  REQUIRE(applySessionOption(&opts, "ConnectTimeout=10"));
  REQUIRE(opts.timeout == 10);
  REQUIRE(applySessionOption(&opts, "ServerAliveInterval=5"));
  REQUIRE(opts.server_alive_interval == 5);
  REQUIRE(applySessionOption(&opts, "ClearAllForwardings=yes"));
  REQUIRE(opts.clear_all_forwardings == 1);
  REQUIRE(applySessionOption(&opts, "ExitOnForwardFailure=yes"));
  REQUIRE(opts.exit_on_forward_failure == 1);
  REQUIRE(applySessionOption(&opts, "BatchMode=yes"));
  REQUIRE(opts.batch_mode == 1);
  REQUIRE(applySessionOption(&opts, "RemoteCommand=echo hi"));
  REQUIRE(string(opts.remote_command) == "echo hi");
  REQUIRE(applySessionOption(&opts, "ControlMaster=auto"));
  REQUIRE(string(opts.control_master) == "auto");
  REQUIRE(applySessionOption(&opts, "ControlPath=/tmp/cm"));
  REQUIRE(string(opts.control_path) == "/tmp/cm");
  REQUIRE(applySessionOption(&opts, "ControlPersist=yes"));
  REQUIRE(string(opts.control_persist) == "yes");
  REQUIRE(applySessionOption(&opts, "Port=2222"));
  REQUIRE(opts.port == 2222);
  REQUIRE(applySessionOption(&opts, "User=alice"));
  REQUIRE(string(opts.username) == "alice");
  freeOptionsFields(&opts);
}

TEST_CASE("OpenSSH -G dump prints required keywords", "[OpenSshLocalQueries]") {
  Options opts = {};
  REQUIRE(applySessionOption(&opts, "ConnectTimeout=10"));
  REQUIRE(applySessionOption(&opts, "ServerAliveInterval=5"));
  REQUIRE(applySessionOption(&opts, "ClearAllForwardings=yes"));
  REQUIRE(applySessionOption(&opts, "ExitOnForwardFailure=yes"));
  REQUIRE(applySessionOption(&opts, "BatchMode=yes"));
  REQUIRE(applySessionOption(&opts, "RemoteCommand=echo hi"));
  REQUIRE(applySessionOption(&opts, "ControlMaster=auto"));
  REQUIRE(applySessionOption(&opts, "ControlPath=/tmp/cm"));
  REQUIRE(applySessionOption(&opts, "ControlPersist=yes"));
  REQUIRE(applySessionOption(&opts, "Port=2222"));

  const string dump =
      formatOpenSshResolvedConfig("alias", "example.com", "alice", opts);
  REQUIRE(dump.find("host alias\n") != string::npos);
  REQUIRE(dump.find("user alice\n") != string::npos);
  REQUIRE(dump.find("hostname example.com\n") != string::npos);
  REQUIRE(dump.find("port 2222\n") != string::npos);
  REQUIRE(dump.find("connecttimeout 10\n") != string::npos);
  REQUIRE(dump.find("serveraliveinterval 5\n") != string::npos);
  REQUIRE(dump.find("clearallforwardings yes\n") != string::npos);
  REQUIRE(dump.find("exitonforwardfailure yes\n") != string::npos);
  REQUIRE(dump.find("batchmode yes\n") != string::npos);
  REQUIRE(dump.find("remotecommand echo hi\n") != string::npos);
  REQUIRE(dump.find("controlmaster auto\n") != string::npos);
  REQUIRE(dump.find("controlpath /tmp/cm\n") != string::npos);
  REQUIRE(dump.find("controlpersist yes\n") != string::npos);
  freeOptionsFields(&opts);
}

TEST_CASE("OpenSSH config file keywords feed -G resolution",
          "[OpenSshLocalQueries][SSHConfig]") {
  const fs::path tempDir =
      fs::temp_directory_path() / ("et_test_openssh_g_" + sole::uuid4().str());
  fs::remove_all(tempDir);
  fs::create_directories(tempDir);
  const fs::path configPath = tempDir / "config";
  std::ofstream(configPath) << "Host demo\n"
                               "  HostName 10.0.0.5\n"
                               "  User bob\n"
                               "  Port 2201\n"
                               "  ConnectTimeout 7\n"
                               "  ServerAliveInterval 3\n"
                               "  BatchMode yes\n"
                               "  RemoteCommand uname -a\n"
                               "  ControlMaster auto\n"
                               "  ControlPath /tmp/et-%r@%h:%p\n"
                               "  ControlPersist 10m\n"
                               "  ClearAllForwardings yes\n"
                               "  ExitOnForwardFailure yes\n";

  Options opts = {};
  ssh_options_set(&opts, SSH_OPTIONS_HOST, "demo");
  REQUIRE(parse_ssh_config_file("demo", &opts, configPath.string()) == 0);
  REQUIRE(string(opts.host) == "10.0.0.5");
  REQUIRE(string(opts.username) == "bob");
  REQUIRE(opts.port == 2201);
  REQUIRE(opts.timeout == 7);
  REQUIRE(opts.server_alive_interval == 3);
  REQUIRE(opts.batch_mode == 1);
  REQUIRE(string(opts.remote_command) == "uname -a");
  REQUIRE(string(opts.control_master) == "auto");
  REQUIRE(string(opts.control_path) == "/tmp/et-%r@%h:%p");
  REQUIRE(string(opts.control_persist) == "10m");
  REQUIRE(opts.clear_all_forwardings == 1);
  REQUIRE(opts.exit_on_forward_failure == 1);

  const string dump =
      formatOpenSshResolvedConfig("demo", opts.host, opts.username, opts);
  REQUIRE(dump.find("connecttimeout 7\n") != string::npos);
  REQUIRE(dump.find("remotecommand uname -a\n") != string::npos);
  REQUIRE(dump.find("controlpath /tmp/et-%r@%h:%p\n") != string::npos);

  freeOptionsFields(&opts);
  fs::remove_all(tempDir);
}

TEST_CASE(
    "applySessionOption rejects invalid ConnectTimeout and ServerAliveInterval",
    "[OpenSshLocalQueries]") {
  Options opts = {};
  REQUIRE_FALSE(applySessionOption(&opts, "ConnectTimeout=garbage"));
  REQUIRE(opts.timeout == 0);
  REQUIRE_FALSE(applySessionOption(&opts, "ConnectTimeout=10foo"));
  REQUIRE(opts.timeout == 0);
  REQUIRE_FALSE(applySessionOption(&opts, "ServerAliveInterval=abc"));
  REQUIRE(opts.server_alive_interval == 0);
  REQUIRE_FALSE(applySessionOption(&opts, "ServerAliveInterval=5x"));
  REQUIRE(opts.server_alive_interval == 0);
  REQUIRE_FALSE(applySessionOption(&opts, "ConnectTimeout=-1"));
  REQUIRE_FALSE(applySessionOption(&opts, "ServerAliveInterval=-3"));

  REQUIRE(applySessionOption(&opts, "ConnectTimeout=none"));
  REQUIRE(opts.timeout == 0);
  REQUIRE(applySessionOption(&opts, "ConnectTimeout=10"));
  REQUIRE(opts.timeout == 10);
  REQUIRE(applySessionOption(&opts, "ServerAliveInterval=5"));
  REQUIRE(opts.server_alive_interval == 5);
  freeOptionsFields(&opts);
}

TEST_CASE("applySessionOption rejects out-of-range and garbage Port",
          "[OpenSshLocalQueries]") {
  Options opts = {};
  REQUIRE_FALSE(applySessionOption(&opts, "Port=70000"));
  REQUIRE(opts.port == 0);
  REQUIRE_FALSE(applySessionOption(&opts, "Port=0"));
  REQUIRE_FALSE(applySessionOption(&opts, "Port=abc"));
  REQUIRE_FALSE(applySessionOption(&opts, "Port=22foo"));
  REQUIRE_FALSE(applySessionOption(&opts, "Port=-1"));

  REQUIRE(applySessionOption(&opts, "Port=65535"));
  REQUIRE(opts.port == 65535);
  REQUIRE(applySessionOption(&opts, "Port=1"));
  REQUIRE(opts.port == 1);
  freeOptionsFields(&opts);
}

TEST_CASE("User config wins over system config for first-seen keywords",
          "[OpenSshLocalQueries][SSHConfig]") {
  const fs::path tempDir = fs::temp_directory_path() /
                           ("et_test_ssh_precedence_" + sole::uuid4().str());
  fs::remove_all(tempDir);
  fs::create_directories(tempDir);
  const fs::path userConfig = tempDir / "user_config";
  const fs::path systemConfig = tempDir / "system_config";

  std::ofstream(userConfig) << "Host demo\n"
                               "  BatchMode no\n"
                               "  ServerAliveInterval 9\n"
                               "  ControlMaster no\n"
                               "  ControlPath /tmp/user-cm\n"
                               "  RemoteCommand from-user\n"
                               "  ConnectTimeout 11\n";
  std::ofstream(systemConfig) << "Host demo\n"
                                 "  BatchMode yes\n"
                                 "  ServerAliveInterval 1\n"
                                 "  ControlMaster auto\n"
                                 "  ControlPath /tmp/system-cm\n"
                                 "  RemoteCommand from-system\n"
                                 "  ConnectTimeout 2\n";

  Options opts = {};
  ssh_options_set(&opts, SSH_OPTIONS_HOST, "demo");
  // Share seen[] across both files so first-wins matches OpenSSH (user over
  // system), including explicit zero/"no" values.
  int seen[SOC_END - SOC_UNSUPPORTED] = {0};
  REQUIRE(parse_ssh_config_file("demo", &opts, userConfig.string(), seen) == 0);
  REQUIRE(parse_ssh_config_file("demo", &opts, systemConfig.string(), seen) ==
          0);

  REQUIRE(opts.batch_mode == 0);
  REQUIRE(opts.server_alive_interval == 9);
  REQUIRE(string(opts.control_master) == "no");
  REQUIRE(string(opts.control_path) == "/tmp/user-cm");
  REQUIRE(string(opts.remote_command) == "from-user");
  REQUIRE(opts.timeout == 11);

  freeOptionsFields(&opts);
  fs::remove_all(tempDir);
}
