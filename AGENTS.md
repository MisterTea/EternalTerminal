# Testing

- To run unit tests: `pushd build; ninja && ctest --parallel --output-on-failure; popd`
- iTerm2, Ghostty, and Hyper e2e tests are opt-in until those terminals' HTM PRs merge. They are not part of default `ctest` / `./et-test`. Run them by name:
  - Ghostty: `pushd build; ./et-test ghostty --reporter compact; popd`
  - iTerm2: `python3 test/system_tests/iterm2_htm_e2e.py --htm build/htm --htmd build/htmd`
  - iTerm2 stress: `python3 test/system_tests/iterm2_htm_stress_e2e.py --htm build/htm --htmd build/htmd`
  - Hyper: from the hyper-htm repo, `npm run test:system`
- To get code coverage: `bash coverage.sh`
- Any time a new test is added, you must run cmake for cmake/ctest to recognize the new test.
- To run lint: `bash format.sh`
- It's important to run lint and unit tests after making changes to the source code.
