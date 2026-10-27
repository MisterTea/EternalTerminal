import sys
import os
import shutil

is_aix = sys.platform == "aix" or sys.platform == "os400"
is_android = os.environ.get("ANDROID_API")
is_x86 = os.environ.get("TEST_X86")
is_arm32 = bool(os.environ.get("TEST_ARM32"))
is_qemu = bool(os.environ.get("TEST_QEMU"))
is_asan = "asan" in os.environ.get("RUN_ANALYZER", "")
is_tsan = "tsan" in os.environ.get("RUN_ANALYZER", "")
is_kcov = "kcov" in os.environ.get("RUN_ANALYZER", "")
is_valgrind = "valgrind" in os.environ.get("RUN_ANALYZER", "")
is_arm64e = "CMAKE_OSX_ARCHITECTURES=arm64e" in os.environ.get("CMAKE_DEFINES", "")

has_http = not is_android and not (sys.platform == "linux" and is_x86)
# breakpad does not work correctly when using kcov or valgrind
# also, asan reports a `stack-buffer-underflow` in breakpad itself,
# and says this may be a false positive due to a custom stack unwinding mechanism
has_breakpad = (
    not is_valgrind
    and not is_kcov
    # Needs porting
    and not is_aix
    # XXX: we support building breakpad, and it captures minidumps when run through sentry-android,
    # however running it from an `adb shell` does not work correctly :-(
    and not is_android
    and not (is_asan and sys.platform == "darwin")
    # breakpad accesses thread state registers directly, which doesn't work on arm64e
    and not (is_arm64e and sys.platform == "darwin")
)
# crashpad requires http, needs porting to AIX, and doesn’t work with kcov/valgrind/tsan either
has_crashpad = (
    has_http
    and not is_valgrind
    and not is_kcov
    and not is_android
    and not is_aix
    and not is_tsan
)
# android has no local filesystem
has_files = not is_android

# OOM tests are unreliable under several conditions:
# - Linux OOM killer sends uncatchable SIGKILL
# - ASAN intercepts malloc
# - Valgrind is too slow with many allocations
has_oom = sys.platform != "linux" and not is_asan and not is_valgrind

# Native backend works on all platforms (lightweight, no external dependencies)
# It's always available - tests explicitly set SENTRY_BACKEND: native in cmake
# On macOS ASAN, the signal handling conflicts with ASAN's memory interception
has_native = has_http and not (is_asan and sys.platform == "darwin")

has_sccache = os.environ.get("USE_SCCACHE") and shutil.which("sccache")
