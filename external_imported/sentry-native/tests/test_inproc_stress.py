import os
import pathlib
import platform
import shutil
import subprocess
import sys
import time

import pytest

from . import adb, Envelope
from .assertions import assert_inproc_crash
from .build_config import get_test_executable_cmake_args, get_test_executable_env
from .conditions import is_android, is_tsan

fixture_path = pathlib.Path("tests/fixtures/inproc_stress")

ANDROID_TMP = "/data/local/tmp"


def compile_test_program(tmp_path):
    build_dir = tmp_path / "inproc_stress_build"
    build_dir.mkdir(exist_ok=True)

    source_dir = pathlib.Path(__file__).parent.parent.resolve()
    include_dir = source_dir / "include"

    cmake_args = get_test_executable_cmake_args(tmp_path, include_dir)
    cmake_args.append(str(fixture_path.resolve()))

    env = get_test_executable_env()

    subprocess.run(
        ["cmake"] + cmake_args,
        check=True,
        cwd=build_dir,
        env=env,
    )

    subprocess.run(
        ["cmake", "--build", ".", "--parallel"],
        check=True,
        cwd=build_dir,
        env=env,
    )

    exe_name = (
        "inproc_stress_test.exe" if sys.platform == "win32" else "inproc_stress_test"
    )
    exe_path = build_dir / exe_name

    # Push executable to Android device
    if is_android:
        adb("push", str(exe_path), ANDROID_TMP, check=True, capture_output=True)

    return exe_path


def run_stress_test(tmp_path, test_executable, test_name, database_path=None):
    if is_android:
        return run_stress_test_android(test_executable, test_name, database_path)

    if database_path is None:
        database_path = tmp_path / ".sentry-native"

    env = os.environ.copy()
    if sys.platform != "win32":
        env["LD_LIBRARY_PATH"] = str(tmp_path) + ":" + env.get("LD_LIBRARY_PATH", "")

    proc = subprocess.Popen(
        [str(test_executable), test_name, str(database_path)],
        cwd=tmp_path,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        encoding="utf-8",
        errors="replace",
    )

    stdout, stderr = proc.communicate(timeout=30)
    return proc.returncode, stdout, stderr


def run_stress_test_android(test_executable, test_name, database_path):
    """Run the stress test on Android via adb shell."""
    exe_name = test_executable.name
    remote_db_path = f"{ANDROID_TMP}/{database_path.name}"

    # Clear logcat before running so we limit the capture as close to this run as possible
    adb("logcat", "-c", check=False)

    # Run on device - we need to capture both stdout and stderr, and the return code
    # Android shell doesn't separate stdout/stderr well, so we redirect stderr to stdout
    # and parse the return code from the output (same approach as tests/__init__.py)
    result = adb(
        "shell",
        f"cd {ANDROID_TMP} && LD_LIBRARY_PATH=. ./{exe_name} {test_name} {remote_db_path} 2>&1; echo ret:$?",
        capture_output=True,
        text=True,
    )

    output = result.stdout
    ret_marker = output.rfind("ret:")
    if ret_marker != -1:
        returncode = int(output[ret_marker + 4 :].strip())
        output = output[:ret_marker]
    else:
        returncode = result.returncode

    # Give the system a moment to flush logcat buffers after the crash
    time.sleep(0.5)

    # Capture logcat to get our logs
    logcat_result = adb(
        "logcat",
        "-d",
        "-s",
        "sentry-native:*",
        capture_output=True,
        text=True,
    )
    logcat_output = logcat_result.stdout

    # Pull the database directory back from the device
    # adb pull creates a subdirectory, so we pull to the parent
    if database_path.exists():
        shutil.rmtree(database_path)

    # Pull the remote database to local path (pulls to parent, creates database_path)
    try:
        adb(
            "pull",
            f"{remote_db_path}/",
            str(database_path.parent),
            check=True,
            capture_output=True,
        )
    except subprocess.CalledProcessError:
        # Database might not exist if crash wasn't captured
        pass

    # Clean up remote database for next run
    adb("shell", f"rm -rf {remote_db_path}", check=False)

    # Combine shell output with logcat output for assertion checks
    combined_output = output + "\n" + logcat_output
    return returncode, combined_output, combined_output


def assert_single_crash_envelope(database_path):
    run_dirs = list(database_path.glob("*.run"))
    assert len(run_dirs) == 1, f"Expected 1 .run directory, found {len(run_dirs)}"

    run_dir = run_dirs[0]
    envelopes = list(run_dir.glob("*.envelope"))
    assert (
        len(envelopes) == 1
    ), f"Expected 1 envelope, found {len(envelopes)}: {envelopes}"

    return envelopes[0]


def assert_crash_marker(database_path):
    assert (database_path / "last_crash").exists(), "Crash marker file missing"


def test_inproc_simple_crash(cmake):
    tmp_path = cmake(
        ["sentry"],
        {"SENTRY_BACKEND": "inproc", "SENTRY_TRANSPORT": "none"},
    )

    test_exe = compile_test_program(tmp_path)
    database_path = tmp_path / ".sentry-native"

    try:
        returncode, stdout, stderr = run_stress_test(
            tmp_path, test_exe, "simple-crash", database_path
        )

        assert returncode != 0, f"Process should have crashed. stderr:\n{stderr}"

        assert (
            "crash has been captured" in stderr
        ), f"Crash not captured. stderr:\n{stderr}"

        assert_crash_marker(database_path)
        envelope_path = assert_single_crash_envelope(database_path)

        with open(envelope_path, "rb") as f:
            envelope = Envelope.deserialize(f.read())
        assert_inproc_crash(envelope)

        event = envelope.get_event()
        assert event is not None, "Event missing from envelope"
        assert "exception" in event, "Exception missing from event"

    finally:
        shutil.rmtree(database_path, ignore_errors=True)


@pytest.mark.skipif(
    is_tsan, reason="disabled in tsan due to triggering edge-case flaky behavior"
)
def test_inproc_concurrent_crash(cmake):
    """
    Stress test: multiple threads crash simultaneously.

    This test verifies that:
    1. Only ONE crash envelope is generated (not one per thread)
    2. The envelope is not corrupted by concurrent access
    3. The process terminates cleanly after handling the first crash
    """
    tmp_path = cmake(
        ["sentry"],
        {"SENTRY_BACKEND": "inproc", "SENTRY_TRANSPORT": "none"},
    )

    test_exe = compile_test_program(tmp_path)
    database_path = tmp_path / ".sentry-native"

    try:
        returncode, stdout, stderr = run_stress_test(
            tmp_path, test_exe, "concurrent-crash", database_path
        )

        assert returncode != 0, f"Process should have crashed. stderr:\n{stderr}"
        assert (
            "crash has been captured" in stderr
        ), f"Crash not captured. stderr:\n{stderr}"

        assert_crash_marker(database_path)
        envelope_path = assert_single_crash_envelope(database_path)

        with open(envelope_path, "rb") as f:
            content = f.read()
            envelope = Envelope.deserialize(content)

        assert_inproc_crash(envelope)

        event = envelope.get_event()
        assert event is not None, "Event missing from envelope"
        assert "exception" in event, "Exception missing from event"

        exc = event["exception"]["values"][0]
        assert "type" in exc, "Exception type missing"
        assert "stacktrace" in exc, "Stacktrace missing from exception"

    finally:
        shutil.rmtree(database_path, ignore_errors=True)


def test_inproc_handler_thread_crash(cmake):
    """
    Test fallback when handler thread crashes via SIGSEGV.

    This test verifies that when the on_crash callback crashes (which runs
    on the handler thread), the signal handler detects this and falls back
    to processing the crash directly in the signal handler context.
    """
    tmp_path = cmake(
        ["sentry"],
        {"SENTRY_BACKEND": "inproc", "SENTRY_TRANSPORT": "none"},
    )

    test_exe = compile_test_program(tmp_path)
    database_path = tmp_path / ".sentry-native"

    try:
        returncode, stdout, stderr = run_stress_test(
            tmp_path, test_exe, "handler-thread-crash", database_path
        )

        assert returncode != 0, f"Process should have crashed. stderr:\n{stderr}"

        assert (
            "on_crash callback about to crash" in stderr
        ), f"on_crash callback didn't run. stderr:\n{stderr}"

        assert (
            "FATAL crash in handler thread" in stderr
        ), f"Handler thread crash not detected. stderr:\n{stderr}"

        assert (
            "crash has been captured" in stderr
        ), f"Crash not captured via fallback. stderr:\n{stderr}"

        assert_crash_marker(database_path)
        envelope_path = assert_single_crash_envelope(database_path)
        with open(envelope_path, "rb") as f:
            envelope = Envelope.deserialize(f.read())
        assert_inproc_crash(envelope)

    finally:
        shutil.rmtree(database_path, ignore_errors=True)


def test_inproc_handler_abort_crash(cmake):
    """
    Test behavior when handler thread crashes via abort().

    When abort() is called from the on_crash callback (running on the handler
    thread), we detect this is a handler thread crash and fall back to
    processing in the signal handler context (with hooks skipped to avoid
    calling the crashing hook again).

    The crash marker should exist (written before on_crash callback), and
    an envelope should be written for the abort crash.
    """
    tmp_path = cmake(
        ["sentry"],
        {"SENTRY_BACKEND": "inproc", "SENTRY_TRANSPORT": "none"},
    )

    test_exe = compile_test_program(tmp_path)
    database_path = tmp_path / ".sentry-native"

    try:
        returncode, stdout, stderr = run_stress_test(
            tmp_path, test_exe, "handler-abort-crash", database_path
        )

        assert returncode != 0, f"Process should have crashed. stderr:\n{stderr}"

        assert (
            "on_crash callback about to abort" in stderr
        ), f"on_crash callback didn't run. stderr:\n{stderr}"

        assert (
            "FATAL crash in handler thread" in stderr
        ), f"Handler thread crash not detected. stderr:\n{stderr}"

        # The abort crash should be captured via the fallback path
        assert (
            "crash has been captured" in stderr
        ), f"Crash should have been captured. stderr:\n{stderr}"

        # Crash marker should exist (written before on_crash callback)
        assert_crash_marker(database_path)

        # An envelope should be written for the abort crash
        run_dirs = list(database_path.glob("*.run"))
        assert len(run_dirs) == 1, f"Expected 1 run dir, found: {run_dirs}"
        envelopes = list(run_dirs[0].glob("*.envelope"))
        assert (
            len(envelopes) == 1
        ), f"Expected 1 envelope after abort(), found: {envelopes}"

    finally:
        shutil.rmtree(database_path, ignore_errors=True)


@pytest.mark.skipif(
    sys.platform != "darwin" or bool(is_android),
    reason="Stack trace tests are macOS-only",
)
@pytest.mark.parametrize(
    "test_name,expected_functions,expect_no_duplicates",
    [
        (
            "stack-no-subcalls",
            [
                "stacktest_B_crash_no_subcalls",
                "stacktest_A_calls_B_no_subcalls",
            ],
            True,
        ),
        (
            "stack-with-subcalls",
            [
                "stacktest_B_crash_after_subcall",
                "stacktest_A_calls_B_with_subcalls",
            ],
            # LR holds a stale return into the crashing function (from the
            # last bl before the crash). We accept this spurious extra frame
            # because skipping LR would lose a real frame in the
            # no-frame-record case, which we can't distinguish at walk time.
            False,
        ),
        pytest.param(
            "stack-no-frame-record",
            [
                "stacktest_B_crash_no_frame_record",
                "stacktest_A_calls_B_no_frame_record",
            ],
            True,
            marks=pytest.mark.skipif(
                platform.machine() not in ("arm64", "aarch64"),
                reason="no-frame-record recovery requires LR (arm64 only)",
            ),
        ),
    ],
    ids=["no-subcalls", "with-subcalls", "no-frame-record"],
)
def test_inproc_stack_trace(cmake, test_name, expected_functions, expect_no_duplicates):
    """
    Test that the fp-based unwinder produces correct stack traces on macOS.

    Verifies:
    - No duplicate frames where avoidable (arm64 LR vs saved LR dedup)
    - No missing frames (functions without frame records need LR)
    - Expected function names appear in the correct order
    """
    tmp_path = cmake(
        ["sentry"],
        {"SENTRY_BACKEND": "inproc", "SENTRY_TRANSPORT": "none"},
    )

    test_exe = compile_test_program(tmp_path)
    database_path = tmp_path / ".sentry-native"

    try:
        returncode, stdout, stderr = run_stress_test(
            tmp_path, test_exe, test_name, database_path
        )

        assert returncode != 0, f"Process should have crashed. stderr:\n{stderr}"
        assert (
            "crash has been captured" in stderr
        ), f"Crash not captured. stderr:\n{stderr}"

        envelope_path = assert_single_crash_envelope(database_path)
        with open(envelope_path, "rb") as f:
            envelope = Envelope.deserialize(f.read())

        event = envelope.get_event()
        assert event is not None, "Event missing from envelope"
        assert "exception" in event, "Exception missing from event"

        exc = event["exception"]["values"][0]
        assert "stacktrace" in exc, "Stacktrace missing from exception"

        frames = exc["stacktrace"]["frames"]
        # Sentry convention: frames are bottom-to-top, so reverse for
        # top-to-bottom (crash frame first)
        func_names = [f.get("function", "") for f in reversed(frames)]

        # Check no consecutive duplicate function names (where expected)
        if expect_no_duplicates:
            for i in range(len(func_names) - 1):
                assert func_names[i] != func_names[i + 1] or not func_names[i], (
                    f"Duplicate consecutive frame: {func_names[i]} at positions"
                    f" {i} and {i+1}.\nAll frames: {func_names}"
                )

        # Check expected functions appear in order
        search_start = 0
        for expected in expected_functions:
            found = False
            for i in range(search_start, len(func_names)):
                if expected in (func_names[i] or ""):
                    found = True
                    search_start = i + 1
                    break
            assert found, (
                f"Expected function '{expected}' not found (after position"
                f" {search_start}) in stack trace.\nAll frames: {func_names}"
            )

    finally:
        shutil.rmtree(database_path, ignore_errors=True)


@pytest.mark.parametrize("iteration", range(5))
@pytest.mark.skipif(
    is_tsan, reason="disabled in tsan due to triggering edge-case flaky behavior"
)
def test_inproc_concurrent_crash_repeated(cmake, iteration):
    """
    minimal assertions version of test_inproc_concurrent_crash that just hits multiple times each CI run.
    """
    tmp_path = cmake(
        ["sentry"],
        {"SENTRY_BACKEND": "inproc", "SENTRY_TRANSPORT": "none"},
    )

    test_exe = compile_test_program(tmp_path)
    database_path = tmp_path / f".sentry-native-{iteration}"

    try:
        returncode, stdout, stderr = run_stress_test(
            tmp_path, test_exe, "concurrent-crash", database_path
        )

        assert returncode != 0, f"Iteration {iteration}: Process should have crashed"
        assert (
            "crash has been captured" in stderr
        ), f"Crash not captured. stderr:\n{stderr}"

        envelope_path = assert_single_crash_envelope(database_path)
        with open(envelope_path, "rb") as f:
            envelope = Envelope.deserialize(f.read())
        assert_inproc_crash(envelope)

    finally:
        shutil.rmtree(database_path, ignore_errors=True)
