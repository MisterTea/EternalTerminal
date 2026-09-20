#include "ParseConfigFile.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("ProxyJump none disables jump host (explicit-none override)",
          "[ProxyJump]") {
  Options opts = {NULL,NULL,NULL,NULL,NULL,NULL,0,0,0,0,0,NULL,NULL,0,0,NULL,{}};
  ssh_options_set(&opts, SSH_OPTIONS_PROXYJUMP, "none");
  REQUIRE(string(opts.ProxyJump) == "none");
  freeOptionsFields(&opts);
}

TEST_CASE("ProxyJump precedence: unset vs set to none vs value",
          "[ProxyJump]") {
  Options opts = {NULL,NULL,NULL,NULL,NULL,NULL,0,0,0,0,0,NULL,NULL,0,0,NULL,{}};
  REQUIRE(opts.ProxyJump == NULL); // unset

  ssh_options_set(&opts, SSH_OPTIONS_PROXYJUMP, "jump.example");
  REQUIRE(string(opts.ProxyJump) == "jump.example");

  ssh_options_set(&opts, SSH_OPTIONS_PROXYJUMP, "none");
  REQUIRE(string(opts.ProxyJump) == "none");

  ssh_options_set(&opts, SSH_OPTIONS_PROXYJUMP, "other");
  REQUIRE(string(opts.ProxyJump) == "other");

  freeOptionsFields(&opts);
}
