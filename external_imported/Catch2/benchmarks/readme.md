<a id="top"></a>

# Benchmarks of Catch2

This folder holds benchmarks for Catch2. It should not be built for the
small(er) test sets, as it is only relevant for running (mainly runtime)
benchmarks.

Below you will find some practical examples using 
[hyperfine](https://github.com/sharkdp/hyperfine) to determine the
performance of various scenarios. They assume two parallel checkouts, one
named `Catch2-old` and the other `Catch2-new`. You will need to change
the paths to work on your own machine.


## Runtime benchmarks

The runtime benchmarks currently consist of few different `TEST_CASE`s,
each with a simple loop over different assertion type. They are compiled
into two binaries, one for assertion slow path and one for assertion fast
path.


### Assumptions and notes about real-world usage

The basic assertion macro, `REQUIRE` (and `CHECK`) is the most common one,
by far. Thus, it is the most important one to run quickly. It is probably
followed by the matcher macro, `REQUIRE_THAT`.

The most common result of an assertion is that it passes. Even if it is
stringified and reported, that is most likely due to some listener/reporter
that wants the string representation, not because it failed.

The performance of both Debug and Release builds are important; users
will run tests in both. LTO runtime performance is not too important,
but compile time perf is.


### Examples

**Compare performance of `REQUIRE` in slow path, debug build**
```text
hyperfine --warmup 2 --shell none --parameter-list version old,new '/home/xarn/benches/Catch2-{version}/build-debug/benchmarks/AssertionsSlowPath -o /dev/null "REQUIRE"'
```

**Compare performance of `REQUIRE_THAT` in fast path, release build**
```text
hyperfine --warmup 2 --shell none --parameter-list version old,new '/home/xarn/benches/Catch2-{version}/build-release/benchmarks/AssertionsFastPath -o /dev/null "REQUIRE_THAT"'
```

**Compare performance of `REQUIRE` with stringification enabled, release build**
```text
hyperfine --warmup 2 --shell none --parameter-list version old,new '/home/xarn/benches/Catch2-{version}/build-release/benchmarks/AssertionsFastPath -s -o /dev/null "REQUIRE"'
```
_Note that we redirect the output to `/dev/null` to reduce the overhead of the actual output printing, to see just the impact of stringification._

TODO:
  * Start empty binary (set up cost base)
  * Start binary with X (100/1k/10k) tests (test registration cost)
  * Section tracking

## Compilation benchmarks

As tests are often iterated upon and relinked, the compilation cost of
Catch2 is also important.


### Examples

**Compare overhead of including `catch_test_macros.hpp`**
```text
hyperfine --warmup 2 --shell none --parameter-list version old,new '/usr/bin/c++  -I/home/xarn/benches/Catch2-{version}/src/catch2/.. -I/home/xarn/benches/Catch2-{version}/build-debug/generated-includes -g -o /dev/null -c /home/xarn/benches/Catch2-{version}/benchmarks/only_include.cpp'
```

**Compare build time of Catch2's `SelfTest` test suite, Debug build**
```text
hyperfine --warmup 2 --parameter-list version old,vas --prepare 'find ~/benches/Catch2-{version}/tests/SelfTest -type f -name "*.cpp" -exec touch {} +' 'ninja -j 1 -C ~/benches/Catch2-{version}/build-debug'
```

TODO:
  * Link-only recipe
