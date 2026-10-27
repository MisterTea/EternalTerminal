"""
Build configuration utilities for sentry-native tests.

This module provides shared logic for reading environment variables and
preparing CMake build parameters, used by both cmake.py (for building
example, library and backend artifacts) and individual tests that need
to build additional executables (like the inproc stress test) but still
need to align with the core artifacts.

This should make it much easier to add additional integration tests that
we no longer want to bloat example.c with.
"""

import os
import shutil
import sys
from pathlib import Path


def get_android_config():
    """
    Returns Android toolchain CMake options if ANDROID_API and ANDROID_NDK
    environment variables are set.
    """
    if not (os.environ.get("ANDROID_API") and os.environ.get("ANDROID_NDK")):
        return {}

    toolchain = "{}/ndk/{}/build/cmake/android.toolchain.cmake".format(
        os.environ["ANDROID_HOME"], os.environ["ANDROID_NDK"]
    )
    return {
        "CMAKE_TOOLCHAIN_FILE": toolchain,
        "ANDROID_ABI": os.environ.get("ANDROID_ARCH") or "x86",
        "ANDROID_NATIVE_API_LEVEL": os.environ["ANDROID_API"],
    }


def get_platform_cmake_args():
    """
    Returns a list of CMake command-line arguments based on platform and
    environment configuration.

    This handles:
    - 32-bit builds (TEST_X86)
    - ASAN/TSAN sanitizers (RUN_ANALYZER)
    - Additional CMAKE_DEFINES from environment
    """
    args = []

    if sys.platform == "win32" and os.environ.get("TEST_X86"):
        args.append("-AWin32")
    elif sys.platform == "linux" and os.environ.get("TEST_X86"):
        args.append("-DSENTRY_BUILD_FORCE32=ON")
    elif sys.platform == "linux" and os.environ.get("TEST_ARM32"):
        args.extend(
            [
                "-DCMAKE_SYSTEM_NAME=Linux",
                "-DCMAKE_SYSTEM_PROCESSOR=arm",
                "-DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc",
                "-DCMAKE_CXX_COMPILER=arm-linux-gnueabihf-g++",
                "-DCMAKE_ASM_COMPILER=arm-linux-gnueabihf-gcc",
            ]
        )

    if "asan" in os.environ.get("RUN_ANALYZER", ""):
        args.append("-DWITH_ASAN_OPTION=ON")

    if "tsan" in os.environ.get("RUN_ANALYZER", ""):
        args.append("-DWITH_TSAN_OPTION=ON")

    if "CMAKE_DEFINES" in os.environ:
        args.extend(os.environ.get("CMAKE_DEFINES").split())

    return args


def get_cflags(extra_cflags=None):
    """
    Returns compiler flags (CFLAGS/CXXFLAGS) for the build.

    This handles:
    - ERROR_ON_WARNINGS -> -Werror
    - MSVC parallel builds and warnings as errors
    - GCC analyzer

    Args:
        extra_cflags: Additional flags to include
    """
    cflags = list(extra_cflags) if extra_cflags else []

    if os.environ.get("ERROR_ON_WARNINGS"):
        cflags.append("-Werror")

    if sys.platform == "win32" and not os.environ.get("TEST_MINGW"):
        cpus = os.cpu_count()
        cflags.append("/WX /MP{}".format(cpus))

    if "gcc" in os.environ.get("RUN_ANALYZER", ""):
        cflags.append("-fanalyzer")

    return cflags


def get_tsan_env():
    """
    Returns TSAN_OPTIONS environment variable value if TSAN is enabled.

    Returns None if TSAN is not enabled.
    """
    if "tsan" not in os.environ.get("RUN_ANALYZER", ""):
        return None

    module_dir = Path(__file__).resolve().parent
    tsan_options = {
        "verbosity": 2,
        "detect_deadlocks": 1,
        "second_deadlock_stack": 1,
        "suppressions": module_dir / "tsan.supp",
    }
    return ":".join(f"{key}={value}" for key, value in tsan_options.items())


def get_test_executable_cmake_args(sentry_lib_dir, sentry_include_dir):
    """
    Returns CMake arguments for building a test executable that links against
    libsentry, with all necessary platform/sanitizer flags.

    Args:
        sentry_lib_dir: Path to directory containing libsentry
        sentry_include_dir: Path to sentry include directory
    """
    args = [
        f"-DSENTRY_LIB_DIR={sentry_lib_dir}",
        f"-DSENTRY_INCLUDE_DIR={sentry_include_dir}",
    ]

    android_config = get_android_config()
    for key, value in android_config.items():
        args.append(f"-D{key}={value}")

    args.extend(get_platform_cmake_args())

    return args


def get_test_executable_cflags():
    """
    Returns CFLAGS for building test executables.

    This is a subset of the flags used for libsentry, focusing on:
    - Sanitizer instrumentation (required for linking with sanitized libsentry)
    - 32-bit builds
    """
    cflags = []

    if sys.platform == "linux" and os.environ.get("TEST_X86"):
        cflags.append("-m32")

    if "asan" in os.environ.get("RUN_ANALYZER", ""):
        cflags.append("-fsanitize=address")

    if "tsan" in os.environ.get("RUN_ANALYZER", ""):
        cflags.append("-fsanitize=thread")

    if "llvm-cov" in os.environ.get("RUN_ANALYZER", ""):
        cflags.extend(["-fprofile-instr-generate", "-fcoverage-mapping"])

    return cflags


def get_test_executable_env():
    """
    Returns environment variables for running/building test executables.
    """
    env = dict(os.environ)

    tsan_opts = get_tsan_env()
    if tsan_opts:
        env["TSAN_OPTIONS"] = tsan_opts

    cflags = get_test_executable_cflags()
    if cflags:
        existing_cflags = env.get("CFLAGS", "")
        existing_cxxflags = env.get("CXXFLAGS", "")
        flags_str = " ".join(cflags)
        env["CFLAGS"] = f"{existing_cflags} {flags_str}".strip()
        env["CXXFLAGS"] = f"{existing_cxxflags} {flags_str}".strip()

    return env
