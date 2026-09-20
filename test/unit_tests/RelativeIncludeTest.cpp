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
