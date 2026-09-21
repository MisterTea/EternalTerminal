#include "TestHeaders.hpp"
#include "src/terminal/PseudoUserTerminalUnix.hpp"
TEST(BackgroundProcessTeardown, SessionHasEndedReflectsChildReaped) {
  PseudoUserTerminal term;
  EXPECT_FALSE(term.sessionHasEnded());
  term.childReaped = true;
  EXPECT_TRUE(term.sessionHasEnded());
}
