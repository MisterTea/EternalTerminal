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

TEST_CASE("subprocess streams SSH banner stderr instead of capturing it",
          "[issue655][issue769]") {
  // Issue #655 needed banner text visible; #769 streams stderr live so
  // Tailscale login URLs appear while ssh is still waiting. Credential
  // stdout stays captured and must not include the banner.
  SubprocessUtils subprocess;
  const string output = subprocess.SubprocessToStringInteractive(
      "sh", {"-c", "printf login-banner >&2; printf IDPASSKEY:ok"});
  REQUIRE(output == "IDPASSKEY:ok");
  REQUIRE(output.find("login-banner") == string::npos);
}
#endif
