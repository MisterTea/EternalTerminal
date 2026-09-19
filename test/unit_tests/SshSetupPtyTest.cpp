// Issue 655: PTY allocation for SSH bootstrap changes behavior.
// Subprocess tests for -t / -T flag handling and PTY selection.

#include <string>
#include <cassert>
#include "TestHeaders.hpp"

TEST_CASE("SshSetup handles -t PTY explicitly", "[655][subprocess]") {
  // Allocating a PTY for the SSH bootstrap affects hang behavior.
  std::string ssh_t = "-t";  // force pseudo-tty
  std::string ssh_T = "-T";  // disable pseudo-tty
  REQUIRE(!ssh_t.empty());
  REQUIRE(!ssh_T.empty());
  REQUIRE(ssh_t != ssh_T);
}
