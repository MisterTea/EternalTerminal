# CI performance and real MemorySanitizer coverage

A PR runs 33 jobs: the macOS MSan job and the duplicate OpenWrt portability
job were removed. The standalone OpenWrt workflow retains the SDK build,
rootfs tests, and package artifact. Each workflow cancels its superseded PR
runs; push runs have independent concurrency groups.

## Why the old MSan checks were green without MSan

The Linux sanitizer workflow added in 2020 did not select Clang. GCC does not
support `-fsanitize=memory`, and the bundled sanitizers-cmake helper only
warned and compiled without it. The macOS workflow copied the same job, but
that helper explicitly turns MSan off outside Linux. The old logs confirmed
both fallbacks. Neither green check represented MemorySanitizer coverage.
Restoring instrumentation also exposed a disabled-telemetry path that
formatted an uninitialized UUID; its identifier is now value-initialized and
a regression assertion checks the buffered value.

CMake now rejects requested MSan on an unsupported OS/compiler or when the
helper cannot enable its flags. The Linux MSan job selects Clang 18 and uses
instrumented libc++/libc++abi from the pinned LLVM 18.1.8 release commit. Its
preflight verifies that normal C++ library use succeeds and an intentional
uninitialized heap read fails with a MemorySanitizer diagnostic.

Target dependencies use an MSan vcpkg triplet and the same C++ runtime.
The libutempter client library is rebuilt with MSan while retaining the
distro's privileged helper. OpenSSL and libsodium use their pinned vcpkg
recipes with assembly disabled
so crypto outputs have instrumented shadow memory. Build tools such as
protoc use the ordinary host triplet. Telemetry is disabled, and the MSan
manifest omits the unused Sentry dependency. Stack traces use glibc's
MSan-intercepted backtrace implementation; the job removes the system
libunwind development header so UST cannot select its raw register-context
assembly. Application stack traces remain available.

To reproduce on 64-bit Linux (x86 or ARM), install Clang/LLVM 18, CMake,
Ninja, the usual build tools, and libutempter development headers, then run:

```sh
export CC=clang-18 CXX=clang++-18
export MSAN_LIBCXX_ROOT="$PWD/.msan-libcxx"
bash .github/ci/msan/build-libcxx.sh
bash .github/ci/msan/check-runtime.sh
bash .github/ci/msan/build-and-test.sh
```

## Cache and concurrency strategy

Dependency caches contain vcpkg's ABI-checked binary archives, rather than
both those archives and duplicate installed trees. Compiler fingerprints,
manifest contents, vcpkg revision, and instrumentation flavor select caches.
Older compatible package archives can be restored and are checked by vcpkg.
Sanitizer, coverage, and system-test jobs can share ordinary dependency
archives; MSan's instrumented archives have a separate flavor.

Successful master push jobs refresh dependency, C++ runtime, and compiler
caches. PRs restore master caches instead of filling the repository's cache
quota with copies scoped to each PR merge ref. GitHub does not permit one PR
to read another PR's cache. The first master run after these changes warms
the new caches; initially cold runs still have to compile dependencies.

Project compilation uses ccache on Unix and pinned sccache on Windows.
Linux defaults to the available CPU count (four on standard public hosted
runners); other platforms retain two workers. `CI_PARALLEL_LEVEL` overrides
the default, and act retains its existing fourteen-worker default. CTest
waits and workflow jobs have time limits so a hang releases runner capacity.
MSan raises the descriptor limit to 4096 for the poller's high-descriptor
regression test.

The previous PR #881 run spent about 38 of 47 Windows minutes building
vcpkg dependencies, while a cached Linux job spent roughly 19 minutes
compiling and four minutes testing. Cache reuse and compile parallelism
address those costs; the unsupported macOS MSan and duplicate OpenWrt jobs
previously consumed about 34 and 31 minutes respectively. These are prior
run measurements, not a claimed speedup benchmark.

## Validation

- Full macOS CTest suite: 585 tests, no failures (four Windows-only skips).
- Full instrumented Linux ARM64 CTest suite: 586 tests, no failures (four
  Windows-only tests and the optional real-tmux comparison skipped).
- MSan runtime preflight accepts initialized libc++ data and detects an
  intentional uninitialized heap read with its expected exit status.
- Unsupported macOS MSan configuration fails immediately with a clear error.
- Changed workflows pass actionlint; shell scripts and composite-action Bash
  steps pass shellcheck; sources pass clang-format 18 and codespell.

A controlled `et-lib` rebuild on the same Linux ARM64 VM (four CPUs, 8 GB RAM),
with the MSan flags unchanged, measured:

| Core library rebuild | Time |
| --- | ---: |
| Two workers, compiler cache disabled | 100.44 s |
| Four workers, compiler cache disabled | 57.49 s |
| Four workers, warmed compiler cache | 1.25 s |

Four workers reduced this compile time by about 43%. This is a core-library
benchmark, not a projection for the complete CI matrix or its queue delay.
