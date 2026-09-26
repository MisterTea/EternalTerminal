// The continuation guard's decision table.
//
// `etctl run` aborts a body the shell parked on, and must never abort a body
// the shell is running.  The second half is the one that bites: an aborted run
// is indistinguishable from a body that never started, so interrupting a
// command that had already begun silently discards its work.  These cases pin
// the asymmetry down.
#include "RunGuard.hpp"
#include "TestHeaders.hpp"

using namespace et;
using et::run_guard::Evidence;
using et::run_guard::shouldDeclareParked;

TEST_CASE("run guard never interrupts a body that started", "[RunGuard]") {
  SECTION("a running command whose output re-armed bracketed paste") {
    // The regression: a themed prompt redrawing, or any program that enables
    // bracketed paste, re-arms paste while the command is still working. That
    // used to read as a continuation prompt and Ctrl-C the command.
    Evidence e;
    e.accepted = true;
    e.rearmed = true;
    CHECK_FALSE(shouldDeclareParked(e));
  }

  SECTION("a long, silent command well past the fallback grace") {
    Evidence e;
    e.accepted = true;
    e.pastFallbackGrace = true;
    e.streamSettled = true;
    CHECK_FALSE(shouldDeclareParked(e));
  }

  SECTION("every signal at once still loses to having started") {
    Evidence e;
    e.accepted = true;
    e.rearmed = true;
    e.pastFallbackGrace = true;
    e.streamSettled = true;
    CHECK_FALSE(shouldDeclareParked(e));
  }
}

TEST_CASE("run guard still catches a shell that ran nothing", "[RunGuard]") {
  SECTION("re-armed paste with nothing executed is a continuation prompt") {
    // zsh parks atomically on a malformed body: it runs nothing, drops paste
    // to evaluate what it got, and re-arms to read the continuation.
    Evidence e;
    e.rearmed = true;
    CHECK(shouldDeclareParked(e));
  }

  SECTION("a shell that parks without touching paste, once settled") {
    Evidence e;
    e.pastFallbackGrace = true;
    e.streamSettled = true;
    CHECK(shouldDeclareParked(e));
  }
}

TEST_CASE("run guard waits for positive evidence", "[RunGuard]") {
  SECTION("the grace alone is not enough while bytes are still arriving") {
    // A slow link can still be delivering the start mark. Acting on the timer
    // alone is what the elapsed-time guard got wrong.
    Evidence e;
    e.pastFallbackGrace = true;
    e.streamSettled = false;
    CHECK_FALSE(shouldDeclareParked(e));
  }

  SECTION("silence before the grace is just a command starting up") {
    Evidence e;
    e.pastFallbackGrace = false;
    e.streamSettled = true;
    CHECK_FALSE(shouldDeclareParked(e));
  }

  SECTION("an OSC-133 session never falls back to the timer") {
    // There is a real mark coming; absence of one is not evidence.
    Evidence e;
    e.oscRead = true;
    e.pastFallbackGrace = true;
    e.streamSettled = true;
    CHECK_FALSE(shouldDeclareParked(e));
  }

  SECTION("an OSC-133 session still trusts a re-arm with no mark") {
    Evidence e;
    e.oscRead = true;
    e.rearmed = true;
    CHECK(shouldDeclareParked(e));
  }

  SECTION("no evidence at all is never parked") {
    CHECK_FALSE(shouldDeclareParked(Evidence{}));
  }
}
