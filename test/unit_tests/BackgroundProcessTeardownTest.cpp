#include "TestHeaders.hpp"

#ifndef WIN32
#include "PseudoUserTerminal.hpp"

namespace et {
class TestPseudoUserTerminal : public PseudoUserTerminal {
 public:
  void setChildReaped(bool reaped) { childReaped = reaped; }
};

TEST_CASE("BackgroundProcessTeardown", "[BackgroundProcessTeardown]") {
  TestPseudoUserTerminal term;
  REQUIRE_FALSE(term.sessionHasEnded());
  term.setChildReaped(true);
  REQUIRE(term.sessionHasEnded());
}
}  // namespace et
#endif
