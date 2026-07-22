# Testing

- To run unit tests: `pushd build; ninja && ctest --parallel --output-on-failure; popd`
- iTerm2, Ghostty, Hyper, WezTerm, and Windows Terminal e2e tests are opt-in until
  those terminals speak tmux -CC against `htm`. They are not part of default
  `ctest` / `./et-test`. Shared suites are `layout`, `stress`, `corners`,
  `affinities`, and `control-plane` (`--suite all` runs every suite). Each
  emulator is a thin driver over the same suite code. Run:
  - Unified: `python3 test/system_tests/htm_gui_e2e.py --emulator wezterm --suite all --htm build/htm --htmd build/htmd`
  - Ghostty all: `python3 test/system_tests/htm_gui_e2e.py --emulator ghostty --suite all --htm build/htm --htmd build/htmd`
  - Ghostty Catch2 (legacy): `pushd build; ./et-test ghostty --reporter compact; popd`
  - Per-emulator defaults are `--suite all`. Targeted wrappers:
    - layout/stress/affinities: `*_htm_e2e.py`, `*_htm_stress_e2e.py`, `*_htm_affinities_e2e.py`
  - iTerm2: `python3 test/system_tests/iterm2_htm_e2e.py --htm build/htm --htmd build/htmd`
  - Hyper: `python3 test/system_tests/hyper_htm_e2e.py --htm build/htm --htmd build/htmd`
  - WezTerm: `python3 test/system_tests/wezterm_htm_e2e.py --htm build/htm --htmd build/htmd`
  - Ghostty: `python3 test/system_tests/ghostty_htm_e2e.py --htm build/htm --htmd build/htmd`
  - Windows Terminal: `py test/system_tests/windows_terminal_htm_e2e.py --wt wtd.exe --htm build/Release/htm.exe --htmd build/Release/htmd.exe`
- To get code coverage: `bash coverage.sh`
- Any time a new test is added, you must run cmake for cmake/ctest to recognize the new test.
- To run lint: `bash format.sh`
- It's important to run lint and unit tests after making changes to the source code.

## Driving etctl against a local et server

To exercise the real et control path (etserver, etterminal, ControlConsole) locally, for example to validate `etctl` run-framing/detection against a specific shell, run a loopback et server. No SSH or network is needed.

```sh
# Build the pieces
cmake --build build --target etctl et etserver etterminal

# A loopback "ssh": et bootstraps by running `ssh <host> '<script>'`, so this
# shim ignores the host and runs that script (always the last arg) locally.
mkdir -p /tmp/etshim
printf '#!/usr/bin/env bash\nfor a in "$@"; do last="$a"; done\nexec /bin/sh -c "$last"\n' > /tmp/etshim/ssh
chmod 755 /tmp/etshim/*

# A local etserver on its default server fifo
build/etserver --port 2022 --logtostdout &

# Open a control session running the target shell natively as a login shell.
# The shell is `$SHELL -l` from the *open* command's environment (the bootstrap
# etterminal forks it), so the connect preamble runs in that shell (no `exec`).
SHELL=/path/to/shell PATH=/tmp/etshim:$PATH \
  build/etctl open sess localhost --port 2022 --terminal-path="$PWD/build/etterminal"

# Drive it, then tear down
build/etctl run sess 'echo hi'
build/etctl key sess eof
kill %1   # the backgrounded etserver
```

Set `SHELL` to the shell under test (`/bin/zsh`, a Homebrew `bash`/`fish`, and so on); the session spawns it fresh as a login shell, so detection and driving are exercised exactly as they would be for a real user of that shell.
