#!/bin/bash
# Manual GUI test: Ctrl+C through a real `tmux -CC` session attached from a
# real iTerm2 window, over a real `et` connection.
#
# Why this cannot be automated headlessly: `tmux -CC` support is only
# exercised end-to-end when an actual GUI terminal (iTerm2) parses the
# control-mode protocol and re-encodes local keystrokes as `send-keys`
# control commands (see src/base/TmuxCcFilter.hpp). The plain-tmux and
# synthetic `tmux -CC` behavior are covered headlessly by
# test/integration_tests/TmuxCtrlCTest.cpp (real tmux, real `yes`, no GUI
# needed); this script instead verifies the piece that only exists once a
# real GUI is in the loop, and needs a human to watch it happen.
#
# Not registered with CTest and not run by default ./et-test or ctest.
# Run this file directly on a macOS machine with iTerm2 installed, logged
# into a GUI session (not SSH-only):
#   ET_BUILD_DIR=./build test/system_tests/tmux_cc_iterm2_manual_test.sh
#
# Skip (exit 77) when not on macOS, no GUI session, or iTerm2 is missing.

set -euo pipefail

SKIP=77
ROOT="$(git rev-parse --show-toplevel)"
BUILD_DIR="${ET_BUILD_DIR:-$ROOT/build}"

skip() {
  echo "SKIP: $1"
  exit "$SKIP"
}

fail() {
  echo "FAIL: $1"
  exit 1
}

if [[ "$(uname -s)" != "Darwin" ]]; then
  skip "this test only exercises the real iTerm2 tmux -CC integration on macOS"
fi

if ! osascript -e 'tell application "System Events" to name of first process' \
  >/dev/null 2>&1; then
  skip "no GUI session available (osascript/System Events unreachable)"
fi

if ! [ -d "/Applications/iTerm.app" ] && ! command -v mdfind >/dev/null 2>&1; then
  skip "iTerm2 not found"
fi
ITERM_APP="/Applications/iTerm.app"
if [ ! -d "$ITERM_APP" ]; then
  FOUND="$(mdfind "kMDItemCFBundleIdentifier == 'com.googlecode.iterm2'" 2>/dev/null | head -1 || true)"
  if [ -z "$FOUND" ]; then
    skip "iTerm2 is not installed"
  fi
  ITERM_APP="$FOUND"
fi

command -v tmux >/dev/null 2>&1 || skip "tmux binary not found in PATH"

ET_BIN="${ET_BIN:-$BUILD_DIR/et}"
ETSERVER_BIN="${ETSERVER_BIN:-$BUILD_DIR/etserver}"
ETTERMINAL_BIN="${ETTERMINAL_BIN:-$BUILD_DIR/etterminal}"
for bin in "$ET_BIN" "$ETSERVER_BIN" "$ETTERMINAL_BIN"; do
  [ -x "$bin" ] || skip "missing build output: $bin (build with BUILD_TESTING=ON first)"
done

cat <<EOF
=================================================================
 MANUAL TEST: Ctrl+C through tmux -CC, attached from real iTerm2
=================================================================

This script cannot fully drive iTerm2's Accessibility layer to confirm
what is on screen, so the final check is a human judgment call. Follow
these steps in the iTerm2 window that is about to open:

  1. In the new iTerm2 window, connect to this machine over et, e.g.:
       ${ET_BIN} localhost
     (or your usual et invocation for a local loopback ssh server).

  2. Once connected, start a tmux control-mode session:
       tmux -CC new-session

     iTerm2 should switch into its native tmux integration (separate
     native window/tabs replace the terminal content).

  3. In the new tmux-integrated pane, run:
       yes ET_MANUAL_TMUX_CC_SPAM

     Let it run for a few seconds so a large backlog builds up.

  4. Press Ctrl+C in that pane.

  5. PASS if the shell prompt reappears within about a second and the
     terminal is immediately responsive (no multi-second stall while
     buffered "ET_MANUAL_TMUX_CC_SPAM" lines drain).
     FAIL if the prompt takes several seconds (or longer) to reappear,
     or old spam keeps scrolling after Ctrl+C.

  6. Clean up: exit the tmux session (Ctrl+D or 'exit'), then exit et.

Opening iTerm2 now...
EOF

open -a "$ITERM_APP"
sleep 2

read -r -p "Press Enter once you have completed steps 1-6 above, or Ctrl+C to abort... " _

read -r -p "Did the prompt return promptly after Ctrl+C (pass/fail)? " RESULT
case "$RESULT" in
  [Pp]*) echo "PASS: manual tmux -CC Ctrl+C check confirmed by operator." ;;
  *) fail "operator reported the prompt did not return promptly" ;;
esac
