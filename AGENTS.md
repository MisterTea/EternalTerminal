# Testing

- To run unit tests: `pushd build; ninja && ctest --parallel --output-on-failure; popd`
- iTerm2, Ghostty, Hyper, and WezTerm e2e tests are opt-in until those terminals speak tmux -CC against `htm`. They are not part of default `ctest` / `./et-test`. Layout and stress suites are shared; each emulator is a driver. Run:
  - Unified: `python3 test/system_tests/htm_gui_e2e.py --emulator wezterm --suite all --htm build/htm --htmd build/htmd`
  - Ghostty layout+stress: `python3 test/system_tests/htm_gui_e2e.py --emulator ghostty --suite all --htm build/htm --htmd build/htmd`
  - Ghostty Catch2 (legacy): `pushd build; ./et-test ghostty --reporter compact; popd`
  - Ghostty layout: `python3 test/system_tests/ghostty_htm_e2e.py --htm build/htm --htmd build/htmd`
  - Ghostty stress: `python3 test/system_tests/ghostty_htm_stress_e2e.py --htm build/htm --htmd build/htmd`
  - iTerm2 layout: `python3 test/system_tests/iterm2_htm_e2e.py --htm build/htm --htmd build/htmd`
  - iTerm2 stress: `python3 test/system_tests/iterm2_htm_stress_e2e.py --htm build/htm --htmd build/htmd`
  - Hyper layout: `python3 test/system_tests/hyper_htm_e2e.py --htm build/htm --htmd build/htmd`
  - Hyper stress: `python3 test/system_tests/hyper_htm_stress_e2e.py --htm build/htm --htmd build/htmd`
  - WezTerm layout: `python3 test/system_tests/wezterm_htm_e2e.py --htm build/htm --htmd build/htmd`
  - WezTerm stress: `python3 test/system_tests/wezterm_htm_stress_e2e.py --htm build/htm --htmd build/htmd`
- To get code coverage: `bash coverage.sh`
- Any time a new test is added, you must run cmake for cmake/ctest to recognize the new test.
- To run lint: `bash format.sh`
- It's important to run lint and unit tests after making changes to the source code.
