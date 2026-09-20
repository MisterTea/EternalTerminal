#include "SubprocessUtils.hpp"
#include "TestHeaders.hpp"

using namespace et;

#ifndef _WIN32
TEST_CASE("subprocess output is drained before waiting for child",
          "[issue655]") {
  SubprocessUtils subprocess;
  const string output = subprocess.SubprocessToStringInteractive(
      "sh", {"-c", "head -c 131072 /dev/zero | tr '\\0' x"});
  REQUIRE(output.size() == 131072);
}

TEST_CASE("subprocess captures stderr used by SSH banners", "[issue655]") {
  SubprocessUtils subprocess;
  const string output = subprocess.SubprocessToStringInteractive(
      "sh", {"-c", "printf login-banner >&2"});
  REQUIRE(output == "login-banner");
}
#endif
