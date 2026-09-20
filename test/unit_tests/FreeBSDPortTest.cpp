#ifndef _WIN32
#include "PseudoUserTerminal.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("login shells use portable argv[0] convention", "[issue683]") {
  REQUIRE(PseudoUserTerminal::loginShellArg0("/bin/sh") == "-sh");
  REQUIRE(PseudoUserTerminal::loginShellArg0("/usr/local/bin/bash") ==
          "-bash");
  REQUIRE(PseudoUserTerminal::loginShellArg0("zsh") == "-zsh");
}
#endif
