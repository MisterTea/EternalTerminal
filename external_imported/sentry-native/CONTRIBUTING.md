# Contribution guidelines

We love and welcome contributions!

In order to maintain a high quality, we run a number of checks on
different OS / Compiler combinations, and using different analysis tools.

## Prerequisites

Building and testing `sentry-native` currently requires the following tools:

- **CMake** and a supported C/C++ compiler, to actually build the code.
- **python** and **pytest**, to run integration tests.
- **clang-format** and **black**, to format the C/C++ and python code respectively.

`pytest` and `black` are installed as virtualenv dependencies automatically.

## Setting up Environment

    $ make setup

This sets up both git, including a pre-commit hook and submodules, and installs
a python virtualenv which is used to run tests and formatting.

## Formatting Code

    $ make format

This should be done automatically as part of the pre-commit hook, but can also
be done manually.

    $ black tests

## Running Tests

    $ make test

Creates a python virtualenv, and runs all the tests through `pytest`.

**Running integration-tests manually**:

    $ pytest --verbose --maxfail=1 --capture=no tests/

When all the python dependencies have been installed, the integration test suite
can also be invoked directly.

The `maxfail` parameter will abort after the first failure, and `capture=no`
will print the complete compiler output, and test log.

**Running unit-tests manually**:

    $ cmake -B build -D CMAKE_RUNTIME_OUTPUT_DIRECTORY=$(pwd)/build
    $ cmake --build build --target sentry_test_unit
    $ ./build/sentry_test_unit

The unit-tests are a separate executable target and can be built and run on
their own.

## How to interpret CI failures

The way that tests are run unfortunately does not make it immediately obvious from
the summary what the actual failure is, especially for compile-time failures.
In such cases, it is good to scan the test output _from top to bottom_ and find
the offending compile error.

When running tests locally, one can use the `--maxfail=1` / `-x` parameter to
abort after the first failure.

## Integration Test Parameters

The integration test suite runs the `sentry_example` target using a variety of
different compile-time parameters, and asserts different use-cases.

Some of its behavior is controlled by env-variables:

- `ERROR_ON_WARNINGS`: Turns on `-Werror` for gcc compatible compilers.
  This is also the default for `MSVC` on windows.
- `RUN_ANALYZER`: Runs the code with/through one or more of the given analyzers.
  This accepts a comma-separated list, and currently has support for:
  - `asan`: Uses clangs AddressSanitizer and runs integration tests with the
    `detect_leaks` flag.
  - `scan-build`: Runs the build through the `scan-build` tool.
  - `code-checker`: Uses the [`CodeChecker`](https://github.com/Ericsson/codechecker)
    tool for builds.
  - `kcov`: Uses [`kcov`](https://github.com/SimonKagstrom/kcov) to collect
    code-coverage statistics.
  - `valgrind`: Uses [`valgrind`](https://valgrind.org/) to check for memory
    issues such as leaks.
  - `gcc`: Use the `-fanalyzer` flag of `gcc > 10`.
    This is currently not stable enough to use, as it leads to false positives
    and internal compiler errors.
- `TEST_X86`: Passes flags to CMake to enable a 32-bit (cross-)compile.
- `ANDROID_API` / `ANDROID_NDK` / `ANDROID_ARCH`: Instructs the test runner to
  build using the given Android `NDK` version, targeting the given `API` and
  `ARCH`. The test runner assumes an already running simulator matching the
  `ARCH`, and will run the tests on that.

**Analyzer Requirements**:

Some tools, such as `kcov` and `valgrind` have their own distribution packages.
Clang-based tools may require an up-to-date clang, and a separate `clang-tools`
packages.
`CodeChecker` has its own
[install instructions](https://github.com/Ericsson/codechecker#install-guide)
with a list of needed dependencies.

**Running examples manually**:

    $ cmake -B build -D CMAKE_RUNTIME_OUTPUT_DIRECTORY=$(pwd)/build
    $ cmake --build build --target sentry_example
    $ ./build/sentry_example log capture-event

The example can be run manually with a variety of commands to test different
scenarios. Additionally, it will use the `SENTRY_DSN` env-variable, and can thus
also be used to capture events/crashes directly to sentry.

The example currently supports the following commends:

- `capture-event`: Captures an event.
- `crash`: Triggers a crash to be captured.
- `log`: Enables debug logging.
- `release-env`: Uses the `SENTRY_RELEASE` env-variable for the release,
  instead of a hardcoded value.
- `attachment`: Adds an attachment, which is currently defined as the
  `CMakeCache.txt` file, which is part of the CMake build folder.
- `stdout`: Uses a custom transport which dumps all envelopes to `stdout`.
- `no-setup`: Skips all scope and breadcrumb initialization code.
- `start-session`: Starts a new release-health session.
- `overflow-breadcrumbs`: Creates a large number of breadcrumbs that overflow
  the maximum allowed number.
- `capture-multiple`: Captures a number of events.
- `sleep`: Introduces a 10 second sleep.
- `add-stacktrace`: Adds the current thread stacktrace to the captured event.
