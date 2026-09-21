#include "Headers.hpp"
#include "TerminalClient.hpp"
#include "TestHeaders.hpp"

using namespace et;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("refreshAgentProxyPath creates and retargets agent proxy socket",
          "[TerminalClient]") {
  const string clientId = "test-" + genRandomAlphaNum(12);
  const string target1 = "/tmp/test-agent-sock-1";
  const string target2 = "/tmp/test-agent-sock-2";

  const fs::path expectedDir =
      fs::path(GetTempDirectory()) / ("et-agent-" + clientId);

  // Clean up any stale directory beforehand
  std::error_code ec;
  fs::remove_all(expectedDir, ec);

  string proxyPath1 = refreshAgentProxyPath(clientId, target1);

#ifdef WIN32
  // On Windows, if symlink creation succeeds, proxyPath1 points to agent.sock.
  // If unprivileged/developer-mode disabled, it safely falls back to target1.
  if (proxyPath1 != target1) {
    REQUIRE_THAT(proxyPath1, ContainsSubstring(clientId));
    REQUIRE_THAT(proxyPath1, ContainsSubstring("agent.sock"));
  }
#else
  REQUIRE_THAT(proxyPath1, ContainsSubstring(clientId));
  REQUIRE_THAT(proxyPath1, ContainsSubstring("agent.sock"));
  REQUIRE(fs::is_symlink(proxyPath1));
  REQUIRE(fs::read_symlink(proxyPath1).string() == target1);

  // Verify secure directory permissions (0700)
  struct stat st{};
  REQUIRE(::stat(expectedDir.c_str(), &st) == 0);
  REQUIRE((st.st_mode & 0777) == (S_IRUSR | S_IWUSR | S_IXUSR));
#endif

  // Re-calling refreshAgentProxyPath simulates retargeting after reconnect
  // (Issue #506)
  string proxyPath2 = refreshAgentProxyPath(clientId, target2);
  REQUIRE(proxyPath1 == proxyPath2);

#ifndef WIN32
  REQUIRE(fs::is_symlink(proxyPath2));
  REQUIRE(fs::read_symlink(proxyPath2).string() == target2);
#endif

  fs::remove_all(expectedDir, ec);
}
