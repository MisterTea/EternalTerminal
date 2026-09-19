#include "SubprocessUtils.hpp"
#include "TestHeaders.hpp"
using namespace et;
TEST_CASE("SSH stderr banner is captured", "[SubprocessUtils]") {
  SubprocessUtils utils;
  string res = utils.SubprocessToStringInteractive("sh", {"-c", "echo banner >&2"});
  REQUIRE(res.find("banner") != string::npos);
}
