#include "ParseConfigFile.hpp"
#include "TestHeaders.hpp"

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
