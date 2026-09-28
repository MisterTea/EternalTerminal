#ifndef __ET_RUN_GUARD_HPP__
#define __ET_RUN_GUARD_HPP__

#include "Headers.hpp"

/*
 * The continuation guard's decision, separated from the loop that feeds it.
 *
 * `etctl run` injects a body and waits for its end marker.  When the body is
 * malformed (an unbalanced quote, an incomplete construct) the shell parks on
 * a continuation prompt and that marker never comes, so the guard recovers
 * with Ctrl-C rather than hanging to the deadline.
 *
 * The two ways of being wrong do not cost the same.  Failing to spot a parked
 * shell costs a slow return: the deadline sends the same Ctrl-C and names the
 * same cause, just later.  Interrupting a shell that is *running* the
 * body kills it partway and discards its output, which for anything
 * non-idempotent is data loss the caller cannot even detect, because an
 * aborted run looks exactly like a body that never started.  The rule below
 * is asymmetric on purpose, and every branch of it is a question about
 * evidence, never about elapsed time alone.
 */
namespace et {
namespace run_guard {

// What the reader has observed on the session since the body was injected.
struct Evidence {
  // The shell provably began running the body: an OSC-133 C or D mark, or the
  // executed start marker in the mark framings.
  bool accepted = false;
  // Bracketed paste went off and then on again, so the line editor evaluated
  // what it received and re-armed itself to read more.
  bool rearmed = false;
  // The fallback grace has elapsed, for a shell that parks without touching
  // bracketed paste at all.
  bool pastFallbackGrace = false;
  // Nothing has arrived for long enough that whatever was in flight landed.
  bool streamSettled = false;
  // This session reads exit codes from OSC 133 rather than echo markers.
  bool oscRead = false;
};

inline bool shouldDeclareParked(const Evidence& e) {
  /*
   * Once the body has started it belongs to the shell.  Nothing observable
   * from here separates a shell that parked *after* running the leading marker
   * (bash reads that line, hits the unterminated construct, and re-prompts on
   * PS2) from a running command whose own output re-armed paste (a themed
   * prompt redrawing, a full-screen app).  Both show the marker and then a
   * re-arm, in that order.  So the tie goes to the command.
   */
  if (e.accepted) return false;
  // Nothing ran, so there is nothing to lose by interrupting: the shell
  // re-prompted without executing the body.
  if (e.rearmed) return true;
  /*
   * A shell that parks atomically never toggles paste, so silence is the only
   * signal left.  Require the stream to have settled as well as the timer to
   * have elapsed, so a mark still in flight on a slow link is not mistaken for
   * a shell that ran nothing.  An OSC-133 session always has a real mark
   * coming and never falls back to this.
   */
  return !e.oscRead && e.pastFallbackGrace && e.streamSettled;
}

}  // namespace run_guard
}  // namespace et

#endif  // __ET_RUN_GUARD_HPP__
