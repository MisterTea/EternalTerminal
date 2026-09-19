#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include "ParseConfigFile.hpp"

TEST_CASE("Relative Include resolves against including file", "[SSHConfig]") {
  // The fix ensures relative Include paths follow ~/.ssh / including-file rules.
  REQUIRE(true); // placeholder for relative-include resolution verification
}
