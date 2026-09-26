#include "MuxProtocol.hpp"
#include "OpenSshLocalQueries.hpp"
#include "ParseConfigFile.hpp"
#include "SocksUtils.hpp"
#include "TestHeaders.hpp"
#include "TunnelUtils.hpp"

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

TEST_CASE("applySessionOption trims spaces around = for Hostname and User",
          "[OpenSshLocalQueries]") {
  Options opts = {};
  REQUIRE(applySessionOption(&opts, "Hostname = 198.51.100.7"));
  REQUIRE(string(opts.host) == "198.51.100.7");
  REQUIRE(applySessionOption(&opts, "User = spaced"));
  REQUIRE(string(opts.username) == "spaced");
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

TEST_CASE("ssh_config forwards keep bind address, target host, and SendEnv",
          "[SSHConfig]") {
  const fs::path tempDir =
      fs::temp_directory_path() / ("et_test_ssh_fwd_" + sole::uuid4().str());
  fs::remove_all(tempDir);
  fs::create_directories(tempDir);
  const fs::path configPath = tempDir / "config";
  std::ofstream(configPath)
      << "Host demo\n"
         "  LocalForward 127.0.0.1:8080 db.internal:5432\n"
         "  LocalForward [::1]:9090 [2001:db8::1]:443\n"
         "  LocalForward *:70 example.com:70\n"
         "  RemoteForward 0.0.0.0:2222 localhost:22\n"
         "  DynamicForward 127.0.0.1:1080\n"
         "  DynamicForward *:1081\n"
         "  SendEnv LANG LC_*\n"
         "  SendEnv -LC_COLLATE\n"
         "  SetEnv TERM=xterm-256color\n"
         "  RemoteCommand tmux attach\n"
         "  ExitOnForwardFailure yes\n"
         "  ServerAliveInterval 2\n"
         "  BatchMode yes\n";

  Options opts = {};
  ssh_options_set(&opts, SSH_OPTIONS_HOST, "demo");
  REQUIRE(parse_ssh_config_file("demo", &opts, configPath.string()) == 0);
  REQUIRE(opts.local_forwards.size() == 3);
  REQUIRE(opts.local_forwards[0] == "127.0.0.1:8080:db.internal:5432");
  REQUIRE(opts.local_forwards[1] == "[::1]:9090:[2001:db8::1]:443");
  REQUIRE(opts.local_forwards[2] == "0.0.0.0:70:example.com:70");
  REQUIRE(opts.remote_forwards.size() == 1);
  REQUIRE(opts.remote_forwards[0] == "0.0.0.0:2222:localhost:22");
  REQUIRE(opts.dynamic_forwards.size() == 2);
  REQUIRE(opts.dynamic_forwards[0] == "127.0.0.1:1080");
  REQUIRE(opts.dynamic_forwards[1] == "0.0.0.0:1081");
  REQUIRE(opts.send_env.size() == 2);
  REQUIRE(opts.send_env[0] == "LANG");
  REQUIRE(opts.send_env[1] == "LC_*");
  REQUIRE(opts.env_vars.size() == 1);
  REQUIRE(opts.env_vars[0].first == "TERM");
  REQUIRE(opts.env_vars[0].second == "xterm-256color");
  REQUIRE(opts.exit_on_forward_failure == 1);
  REQUIRE(opts.batch_mode == 1);
  REQUIRE(string(opts.remote_command) == "tmux attach");

  const string dump =
      formatOpenSshResolvedConfig("demo", opts.host, "demo", opts);
  REQUIRE(dump.find("localforward 127.0.0.1:8080:db.internal:5432\n") !=
          string::npos);
  REQUIRE(dump.find("remoteforward 0.0.0.0:2222:localhost:22\n") !=
          string::npos);
  REQUIRE(dump.find("dynamicforward 0.0.0.0:1081\n") != string::npos);
  REQUIRE(dump.find("sendenv LC_*\n") != string::npos);
  REQUIRE(dump.find("setenv TERM=xterm-256color\n") != string::npos);

  freeOptionsFields(&opts);
  fs::remove_all(tempDir);
}

TEST_CASE("ClearAllForwardings yes drops config and is visible to -G",
          "[SSHConfig]") {
  const fs::path tempDir =
      fs::temp_directory_path() / ("et_test_ssh_clear_" + sole::uuid4().str());
  fs::remove_all(tempDir);
  fs::create_directories(tempDir);
  const fs::path configPath = tempDir / "config";
  std::ofstream(configPath) << "Host demo\n"
                               "  LocalForward 8080 localhost:80\n"
                               "  RemoteForward 90 localhost:9\n"
                               "  DynamicForward 1080\n"
                               "  ClearAllForwardings yes\n"
                               "  LocalForward 8081 localhost:81\n";

  Options opts = {};
  ssh_options_set(&opts, SSH_OPTIONS_HOST, "demo");
  REQUIRE(parse_ssh_config_file("demo", &opts, configPath.string()) == 0);
  REQUIRE(opts.local_forwards.size() == 2);
  REQUIRE(opts.clear_all_forwardings == 1);
  applyClearAllForwardings(&opts);
  REQUIRE(opts.local_forwards.empty());
  REQUIRE(opts.remote_forwards.empty());
  REQUIRE(opts.dynamic_forwards.empty());

  const string dump = formatOpenSshResolvedConfig("demo", "demo", "demo", opts);
  REQUIRE(dump.find("clearallforwardings yes\n") != string::npos);
  REQUIRE(dump.find("localforward ") == string::npos);

  freeOptionsFields(&opts);
  fs::remove_all(tempDir);
}

TEST_CASE("applySessionOption accepts forwards and SendEnv",
          "[OpenSshLocalQueries]") {
  Options opts = {};
  REQUIRE(applySessionOption(&opts, "LocalForward=127.0.0.1:8080:db:5432"));
  REQUIRE(opts.local_forwards.size() == 1);
  REQUIRE(opts.local_forwards[0] == "127.0.0.1:8080:db:5432");
  REQUIRE(applySessionOption(&opts, "RemoteForward=2222 localhost:22"));
  REQUIRE(opts.remote_forwards[0] == "2222:localhost:22");
  REQUIRE(applySessionOption(&opts, "DynamicForward=1080"));
  REQUIRE(opts.dynamic_forwards[0] == "1080");
  REQUIRE(applySessionOption(&opts, "SendEnv=LANG LC_*"));
  REQUIRE(applySessionOption(&opts, "SendEnv=-LC_*"));
  REQUIRE(opts.send_env.size() == 1);
  REQUIRE(opts.send_env[0] == "LANG");
  REQUIRE_FALSE(applySessionOption(&opts, "RemoteForward=1080"));
  REQUIRE_FALSE(applySessionOption(&opts, "SendEnv=FOO=bar"));
  REQUIRE(applySessionOption(&opts, "ClearAllForwardings=yes"));
  applyClearAllForwardings(&opts);
  REQUIRE(opts.local_forwards.empty());
  REQUIRE(opts.remote_forwards.empty());
  REQUIRE(opts.dynamic_forwards.empty());
  freeOptionsFields(&opts);
}

TEST_CASE("applySessionOption rejects listen-only LocalForward/RemoteForward",
          "[OpenSshLocalQueries][SSHConfig]") {
  Options opts = {};
  // Listen-only / remote-SOCKS forms have a colon but no connect target.
  REQUIRE_FALSE(applySessionOption(&opts, "RemoteForward=127.0.0.1:1080"));
  REQUIRE(opts.remote_forwards.empty());
  REQUIRE_FALSE(applySessionOption(&opts, "LocalForward=127.0.0.1:8080"));
  REQUIRE(opts.local_forwards.empty());
  REQUIRE_FALSE(applySessionOption(&opts, "RemoteForward=1080"));
  REQUIRE_FALSE(applySessionOption(&opts, "LocalForward=8080"));
  // Incomplete port:host (no hostport) is also rejected.
  REQUIRE_FALSE(applySessionOption(&opts, "LocalForward=8080:localhost"));
  REQUIRE(opts.local_forwards.empty());

  // Complete specs still accepted, including ET port:port and unix pairs.
  REQUIRE(applySessionOption(&opts, "LocalForward=8080:80"));
  REQUIRE(opts.local_forwards.back() == "8080:80");
  REQUIRE(applySessionOption(&opts, "LocalForward=8080:localhost:80"));
  REQUIRE(opts.local_forwards.back() == "8080:localhost:80");
  REQUIRE(applySessionOption(&opts, "LocalForward=127.0.0.1:8080:db:5432"));
  REQUIRE(opts.local_forwards.back() == "127.0.0.1:8080:db:5432");
  REQUIRE(
      applySessionOption(&opts, "RemoteForward=[::1]:9090:[2001:db8::1]:443"));
  REQUIRE(opts.remote_forwards.back() == "[::1]:9090:[2001:db8::1]:443");
  REQUIRE(applySessionOption(&opts, "LocalForward=/tmp/a.sock:/tmp/b.sock"));
  REQUIRE(opts.local_forwards.back() == "/tmp/a.sock:/tmp/b.sock");
  freeOptionsFields(&opts);
}

TEST_CASE(
    "applySessionOption rejects socket LocalForward forms tunnel parser "
    "rejects",
    "[OpenSshLocalQueries][SSHConfig]") {
  Options opts = {};
  // OpenSSH LocalForward /tmp/a.sock localhost:80 becomes socket:host:hostport,
  // which parseRangesToRequests rejects (3-field requires port:host:hostport).
  REQUIRE_FALSE(
      applySessionOption(&opts, "LocalForward=/tmp/a.sock:localhost:80"));
  REQUIRE(opts.local_forwards.empty());
  // socket:hostname is not a valid two-field ET socket form either.
  REQUIRE_FALSE(applySessionOption(&opts, "LocalForward=/tmp/a.sock:hostname"));
  REQUIRE(opts.local_forwards.empty());

  // Accepted specs must still parse without throwing.
  REQUIRE(applySessionOption(&opts, "LocalForward=/tmp/a.sock:/tmp/b.sock"));
  REQUIRE_NOTHROW(parseRangesToRequests(opts.local_forwards.back()));
  REQUIRE(applySessionOption(&opts, "LocalForward=/tmp/a.sock:8080"));
  REQUIRE_NOTHROW(parseRangesToRequests(opts.local_forwards.back()));
  freeOptionsFields(&opts);
}

TEST_CASE(
    "applySessionOption rejects port-range LocalForward forms tunnel "
    "parser rejects",
    "[OpenSshLocalQueries][SSHConfig]") {
  Options opts = {};
  // One-sided ranges and unequal lengths fail in processEtStyleTunnelArg;
  // ssh_options_set / -o must reject them at option time.
  REQUIRE_FALSE(applySessionOption(&opts, "LocalForward=8000-8002:9000"));
  REQUIRE(opts.local_forwards.empty());
  REQUIRE_FALSE(applySessionOption(&opts, "LocalForward=8000:9000-9002"));
  REQUIRE(opts.local_forwards.empty());
  REQUIRE_FALSE(applySessionOption(&opts, "LocalForward=8000-8002:9000-9001"));
  REQUIRE(opts.local_forwards.empty());

  // Equal-length ranges are accepted by processEtStyleTunnelArg.
  REQUIRE(applySessionOption(&opts, "LocalForward=8000-8002:9000-9002"));
  REQUIRE(opts.local_forwards.back() == "8000-8002:9000-9002");
  REQUIRE_NOTHROW(parseRangesToRequests(opts.local_forwards.back()));
  freeOptionsFields(&opts);
}

TEST_CASE("applySessionOption SetEnv overrides matching SendEnv",
          "[OpenSshLocalQueries]") {
  Options opts = {};
  REQUIRE(applySessionOption(&opts, "SetEnv=TERM=xterm-256color"));
  REQUIRE(opts.env_vars.size() == 1);
  REQUIRE(opts.env_vars[0].first == "TERM");
  REQUIRE(opts.env_vars[0].second == "xterm-256color");

  REQUIRE(applySessionOption(&opts, "SendEnv=TERM"));
  REQUIRE(opts.send_env.size() == 1);
  REQUIRE(opts.send_env[0] == "TERM");

  vector<pair<string, string>> localEnv = {{"TERM", "dumb"}, {"OTHER", "x"}};
  auto merged = mergeSessionEnvironment(opts.send_env, opts.env_vars, localEnv);
  string term;
  for (const auto& env : merged) {
    if (env.first == "TERM") {
      term = env.second;
    }
  }
  REQUIRE(term == "xterm-256color");
  freeOptionsFields(&opts);
}

TEST_CASE("applySessionOption accepts space-form SetEnv NAME=VALUE",
          "[OpenSshLocalQueries][SSHConfig]") {
  Options opts = {};
  // OpenSSH -o "SetEnv NAME=VALUE" (space form). Values always contain '='.
  REQUIRE(applySessionOption(&opts, "SetEnv TERM=xterm-256color"));
  REQUIRE(opts.env_vars.size() == 1);
  REQUIRE(opts.env_vars[0].first == "TERM");
  REQUIRE(opts.env_vars[0].second == "xterm-256color");
  freeOptionsFields(&opts);
}

TEST_CASE("applySessionOption rejects invalid DynamicForward specs",
          "[OpenSshLocalQueries][SSHConfig]") {
  Options opts = {};
  REQUIRE_FALSE(applySessionOption(&opts, "DynamicForward=junk"));
  REQUIRE(opts.dynamic_forwards.empty());
  REQUIRE_FALSE(applySessionOption(&opts, "DynamicForward=99999"));
  REQUIRE(opts.dynamic_forwards.empty());
  // Bind without a port is not a valid -D listen spec.
  REQUIRE_FALSE(applySessionOption(&opts, "DynamicForward=localhost"));
  REQUIRE(opts.dynamic_forwards.empty());

  REQUIRE(applySessionOption(&opts, "DynamicForward=1080"));
  REQUIRE(opts.dynamic_forwards.back() == "1080");
  REQUIRE_NOTHROW(parseDynamicForwardArg(opts.dynamic_forwards.back()));
  REQUIRE(applySessionOption(&opts, "DynamicForward=127.0.0.1:9050"));
  REQUIRE(opts.dynamic_forwards.back() == "127.0.0.1:9050");
  REQUIRE_NOTHROW(parseDynamicForwardArg(opts.dynamic_forwards.back()));
  freeOptionsFields(&opts);
}

TEST_CASE("applySessionOption rejects oversized LocalForward ports",
          "[OpenSshLocalQueries][SSHConfig]") {
  Options opts = {};
  // Digit-only but too large for a TCP port / for stoi → must reject, not
  // throw.
  REQUIRE_FALSE(applySessionOption(&opts, "LocalForward=9999999999:80"));
  REQUIRE(opts.local_forwards.empty());
  REQUIRE_FALSE(applySessionOption(
      &opts, "LocalForward=8000-9999999999:9000-9999999999"));
  REQUIRE(opts.local_forwards.empty());
  REQUIRE_FALSE(applySessionOption(&opts, "LocalForward=70000:80"));
  REQUIRE(opts.local_forwards.empty());

  REQUIRE(applySessionOption(&opts, "LocalForward=8080:80"));
  REQUIRE_NOTHROW(parseRangesToRequests(opts.local_forwards.back()));
  REQUIRE(applySessionOption(&opts, "LocalForward=8000-8002:9000-9002"));
  REQUIRE_NOTHROW(parseRangesToRequests(opts.local_forwards.back()));
  freeOptionsFields(&opts);
}

TEST_CASE("SendEnv selection and SetEnv override", "[OpenSshLocalQueries]") {
  vector<pair<string, string>> localEnv = {
      {"LANG", "en_US.UTF-8"},
      {"LC_ALL", "C"},
      {"LC_COLLATE", "C"},
      {"OTHER", "x"},
  };
  vector<string> patterns = {"LANG", "LC_*"};
  auto selected = selectSendEnv(patterns, localEnv);
  REQUIRE(selected.size() == 3);

  vector<pair<string, string>> setenv = {{"LANG", "C.UTF-8"}, {"FOO", "bar"}};
  auto merged = mergeSessionEnvironment(patterns, setenv, localEnv);
  string lang;
  string foo;
  for (const auto& env : merged) {
    if (env.first == "LANG") {
      lang = env.second;
    }
    if (env.first == "FOO") {
      foo = env.second;
    }
  }
  REQUIRE(lang == "C.UTF-8");
  REQUIRE(foo == "bar");
  REQUIRE(sshEnvPatternMatches("LC_*", "LC_ALL"));
  REQUIRE_FALSE(sshEnvPatternMatches("LC_*", "LANG"));
}

TEST_CASE("keepalive, remote command, and BatchMode resolution",
          "[OpenSshLocalQueries]") {
  REQUIRE(resolveEtKeepaliveSeconds(true, 2, 30) == 2);
  REQUIRE(resolveEtKeepaliveSeconds(false, 5, 0) == 5);
  REQUIRE(resolveEtKeepaliveSeconds(false, 5, 2) == 2);
  REQUIRE(resolveEtKeepaliveSeconds(false, 5, 60) ==
          MAX_CLIENT_KEEP_ALIVE_DURATION);

  REQUIRE(resolveConfiguredRemoteCommand("from-cli", "from-config") ==
          "from-cli");
  REQUIRE(resolveConfiguredRemoteCommand("", "from-config") == "from-config");
  REQUIRE(resolveConfiguredRemoteCommand("", "none").empty());
  REQUIRE(resolveConfiguredRemoteCommand("", nullptr).empty());

  // -N suppresses config RemoteCommand (OpenSSH SessionType none); no conflict.
  REQUIRE(resolveConfiguredRemoteCommand("", "from-config", true).empty());
  REQUIRE_FALSE(remoteCommandConflictsWithNoCommand(
      true, resolveConfiguredRemoteCommand("", "from-config", true)));
  // A real CLI command with -N still surfaces for the conflict check.
  REQUIRE(resolveConfiguredRemoteCommand("cli-cmd", "from-config", true) ==
          "cli-cmd");
  REQUIRE(remoteCommandConflictsWithNoCommand(
      true, resolveConfiguredRemoteCommand("cli-cmd", "from-config", true)));

  vector<string> sshOptions = {"Compression=yes"};
  appendBatchModeSshOption(&sshOptions, 1);
  REQUIRE(sshOptions.size() == 2);
  REQUIRE(sshOptions[1] == "BatchMode=yes");
  appendBatchModeSshOption(&sshOptions, 1);
  REQUIRE(sshOptions.size() == 2);
  vector<string> untouched;
  appendBatchModeSshOption(&untouched, 0);
  REQUIRE(untouched.empty());
}
