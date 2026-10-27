"""
End-to-end integration tests that verify Sentry receives crash events.

These tests send real crash events to Sentry and verify they arrive with
the expected structure for all crash backends and native crash reporting modes.

Requires environment variables:
- SENTRY_E2E_DSN: Sentry test project DSN
- SENTRY_E2E_AUTH_TOKEN: Sentry API token with project:read scope
- SENTRY_E2E_ORG: Sentry organization slug
- SENTRY_E2E_PROJECT: Sentry project slug

Skip these tests if env vars not set (for regular CI runs).
"""

import os
import re
import subprocess
import sys
import time
from urllib.parse import urlsplit, urlunsplit

import pytest
import requests

from . import run, check_output
from .conditions import (
    has_breakpad,
    has_crashpad,
    has_http,
    has_native,
    is_asan,
)

# Skip all tests if E2E env vars not configured
pytestmark = [
    pytest.mark.skipif(
        not os.environ.get("SENTRY_E2E_DSN"),
        reason="E2E tests require SENTRY_E2E_DSN environment variable",
    ),
    pytest.mark.skipif(
        not has_http,
        reason="E2E tests require http transport",
    ),
]

POLL_MAX_ATTEMPTS = 100
POLL_INTERVAL = 6  # seconds


def get_sentry_api_base():
    """Build the Sentry API base URL from a project DSN."""
    dsn = os.environ["SENTRY_E2E_DSN"]
    try:
        parsed = urlsplit(dsn)
        if (
            not parsed.scheme
            or not parsed.hostname
            or (parsed.port is not None and parsed.port < 0)
        ):
            raise ValueError
    except ValueError:
        pytest.skip(f"SENTRY_E2E_DSN is invalid: {dsn!r}")

    netloc = parsed.netloc.rsplit("@", 1)[-1]
    # Ingest DSNs can be "<org>.ingest.<api-host>"", API calls go to <api-host>
    netloc = re.sub(r"^[^.]+\.ingest\.", "", netloc)
    prefix = parsed.path.rstrip("/").rsplit("/", 1)[0]
    return urlunsplit((parsed.scheme, netloc, f"{prefix}/api/0", "", ""))


def get_sentry_headers():
    """Get authorization headers for Sentry API."""
    token = os.environ.get("SENTRY_E2E_AUTH_TOKEN")
    if not token:
        pytest.skip("SENTRY_E2E_AUTH_TOKEN not set")
    return {"Authorization": f"Bearer {token}"}


def get_sentry_org_project():
    """Get organization and project slugs from environment."""
    org = os.environ.get("SENTRY_E2E_ORG")
    project = os.environ.get("SENTRY_E2E_PROJECT")
    if not org or not project:
        pytest.skip("SENTRY_E2E_ORG and SENTRY_E2E_PROJECT must be set")
    return org, project


def poll_sentry_for_event(test_id, max_attempts=POLL_MAX_ATTEMPTS):
    """
    Poll Sentry API until event with test.id tag appears.

    Uses a two-step approach:
    1. Search issues by test.id tag to find the containing issue
    2. Query that issue's events with the same tag to get the exact event

    Args:
        test_id: The unique test ID tag value to search for
        max_attempts: Maximum number of polling attempts

    Returns:
        The full event data from Sentry API

    Raises:
        TimeoutError: If event not found within max attempts
    """
    org, project = get_sentry_org_project()
    headers = get_sentry_headers()
    api_base = get_sentry_api_base()

    last_error = None

    print(f"\nWaiting for event with test.id={test_id} to appear in Sentry...")
    print(f"Using api_base={api_base}, org={org}, project={project}")

    for attempt in range(max_attempts):
        try:
            # Step 1: Search issues by test.id tag
            issues_url = f"{api_base}/projects/{org}/{project}/issues/"
            response = requests.get(
                issues_url,
                headers=headers,
                params={"query": f"test.id:{test_id}"},
                timeout=30,
            )

            print(
                f"  Attempt {attempt + 1}/{max_attempts}: "
                f"issues search -> {response.status_code}"
            )

            if response.status_code == 200:
                issues = response.json()
                if issues:
                    issue_id = issues[0]["id"]

                    # Step 2: Find the exact event within this issue
                    events_url = f"{api_base}/issues/{issue_id}/events/"
                    event_response = requests.get(
                        events_url,
                        headers=headers,
                        params={"query": f"test.id:{test_id}"},
                        timeout=30,
                    )

                    if event_response.status_code == 200:
                        events = event_response.json()
                        if events:
                            event_id = events[0]["eventID"]
                            print(
                                f"    Found issue {issue_id}, "
                                f"event {event_id}, fetching details..."
                            )

                            # Step 3: Fetch full event detail
                            detail_url = (
                                f"{api_base}/projects/{org}/{project}"
                                f"/events/{event_id}/"
                            )
                            detail_response = requests.get(
                                detail_url, headers=headers, timeout=30
                            )

                            if detail_response.status_code == 200:
                                event = detail_response.json()
                                tags = {
                                    t["key"]: t["value"] for t in event.get("tags", [])
                                }
                                if tags.get("test.id") == test_id:
                                    print(
                                        f"  Found event after "
                                        f"{attempt + 1} attempts"
                                    )
                                    return event
                                else:
                                    last_error = (
                                        f"Tag mismatch: expected {test_id}, "
                                        f"got {tags.get('test.id', '<missing>')}"
                                    )
                            else:
                                last_error = (
                                    f"Event detail returned "
                                    f"{detail_response.status_code}"
                                )
                        else:
                            print(
                                f"    Issue {issue_id} found but "
                                f"event query returned 0 events"
                            )
                    else:
                        last_error = (
                            f"Issue events returned " f"{event_response.status_code}"
                        )
                else:
                    print(f"    No issues found yet")
            else:
                last_error = (
                    f"Issues API returned {response.status_code}: "
                    f"{response.text[:200]}"
                )
                print(f"    Error: {last_error}")

        except requests.RequestException as e:
            last_error = str(e)
            print(f"    Request exception: {last_error}")

        time.sleep(POLL_INTERVAL)

    error_msg = f"Event with test.id={test_id} not found after {max_attempts} attempts"
    if last_error:
        error_msg += f". Last error: {last_error}"
    raise TimeoutError(error_msg)


def get_exception_from_event(event):
    """
    Extract exception data from Sentry API event response.

    The API returns exception data in 'entries' array, not directly on event.
    """
    # Check entries array (Sentry API format)
    entries = event.get("entries", [])
    for entry in entries:
        if entry.get("type") == "exception":
            return entry.get("data", {})

    # Fallback: check direct exception field
    if "exception" in event:
        return event["exception"]

    return None


def get_threads_from_event(event):
    """
    Extract threads data from Sentry API event response.

    The API returns threads data in 'entries' array, not directly on event.
    """
    # Check entries array (Sentry API format)
    entries = event.get("entries", [])
    thread_entries = [e for e in entries if e.get("type") == "threads"]

    # Debug: print if multiple thread entries found
    if len(thread_entries) > 1:
        print(
            f"\n=== WARNING: MULTIPLE THREAD ENTRIES FOUND: {len(thread_entries)} ==="
        )
        for i, te in enumerate(thread_entries):
            data = te.get("data", {})
            values = data.get("values", [])
            print(f"  Entry {i}: {len(values)} threads")
        print("=== END WARNING ===\n")

    if thread_entries:
        return thread_entries[0].get("data", {})

    # Fallback: check direct threads field
    if "threads" in event:
        return event["threads"]

    return None


def verify_no_thread_duplication(threads_data, test_name):
    """
    Verify that thread list has no duplicates.

    This checks for a known issue where the entire thread list appears twice
    on Windows.

    Args:
        threads_data: The threads data dict containing "values" list
        test_name: Name of the test for error messages

    Raises:
        AssertionError: If duplicate thread IDs are found
    """
    if not threads_data or "values" not in threads_data:
        return

    threads = threads_data["values"]
    thread_ids = [t.get("id") for t in threads if t.get("id") is not None]

    # Check for duplicate thread IDs
    seen_ids = set()
    duplicates = []
    for tid in thread_ids:
        if tid in seen_ids:
            duplicates.append(tid)
        seen_ids.add(tid)

    if duplicates:
        # Print detailed thread info for debugging
        print(f"\n=== THREAD DUPLICATION DETECTED in {test_name} ===")
        print(f"Total threads: {len(threads)}")
        print(f"Unique thread IDs: {len(seen_ids)}")
        print(f"Duplicate IDs: {duplicates}")
        print("Thread list:")
        for i, t in enumerate(threads):
            print(
                f"  [{i}] id={t.get('id')}, "
                f"crashed={t.get('crashed')}, "
                f"current={t.get('current')}, "
                f"name={t.get('name', 'N/A')}"
            )
        print("=== END THREAD DUPLICATION DEBUG ===\n")

        raise AssertionError(
            f"Thread duplication detected in {test_name}: "
            f"{len(threads)} total threads but only {len(seen_ids)} unique IDs. "
            f"Duplicate IDs: {duplicates}"
        )


def get_debug_meta_from_event(event):
    """
    Extract debug_meta data from Sentry API event response.

    The API returns debug_meta in 'entries' array with type 'debugmeta'.
    """
    # Check entries array (Sentry API format)
    entries = event.get("entries", [])
    for entry in entries:
        if entry.get("type") == "debugmeta":
            return entry.get("data", {})

    # Fallback: check direct debug_meta field
    if "debug_meta" in event:
        return event["debug_meta"]

    return None


def extract_test_id(output):
    """
    Extract TEST_ID from app output.

    Args:
        output: Bytes output from the test app

    Returns:
        The test ID string (UUID format)

    Raises:
        ValueError: If TEST_ID not found in output
    """
    decoded = output.decode("utf-8", errors="replace")
    match = re.search(r"TEST_ID:([a-f0-9-]{36})", decoded)
    if match:
        return match.group(1)
    raise ValueError(f"TEST_ID not found in output. Output was:\n{decoded[:500]}")


def run_crash_e2e(tmp_path, exe, args, env, wait_for_daemon=False):
    """
    Run a crash test for E2E, capturing output for test ID extraction.

    Handles ASAN signal handling similar to run_crash in test_integration_native.py.
    """
    if is_asan:
        asan_opts = env.get("ASAN_OPTIONS", "")
        asan_signal_opts = (
            "handle_segv=0:handle_sigbus=0:handle_abort=0:"
            "handle_sigfpe=0:handle_sigill=0:allow_user_segv_handler=1"
        )
        if asan_opts:
            env["ASAN_OPTIONS"] = f"{asan_opts}:{asan_signal_opts}"
        else:
            env["ASAN_OPTIONS"] = asan_signal_opts

    # Use check_output to capture stdout for test ID extraction
    return check_output(
        tmp_path,
        exe,
        args,
        env=env,
        expect_failure=True,
        wait_for_daemon=wait_for_daemon,
    )


@pytest.mark.skipif(
    not has_native,
    reason="E2E crash mode tests require native backend",
)
class TestE2ENative:
    """E2E tests for all 3 crash reporting modes against real Sentry."""

    @pytest.fixture(autouse=True)
    def setup(self, cmake):
        """Build the test app and set up DSN."""
        self.tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})
        self.dsn = os.environ["SENTRY_E2E_DSN"]

    def print_daemon_logs(self):
        """Print daemon log files from the database directory for debugging."""
        import glob
        from pathlib import Path

        # Find .sentry-native directory in tmp_path
        db_path = Path(self.tmp_path) / ".sentry-native"
        if not db_path.exists():
            print(f"\n=== No .sentry-native directory found at {db_path} ===")
            return

        # Find daemon log files
        log_files = list(db_path.glob("sentry-daemon-*.log"))
        if not log_files:
            print(f"\n=== No daemon log files found in {db_path} ===")
            return

        for log_file in log_files:
            print(f"\n=== DAEMON LOG: {log_file.name} ===")
            try:
                content = log_file.read_text(errors="replace")
                print(content)
            except Exception as e:
                print(f"Error reading log: {e}")
            print(f"=== END DAEMON LOG ===\n")

    def run_crash_and_send(self, mode_args):
        """
        Crash the app with given mode, then restart to send the pending crash.

        Args:
            mode_args: List of crash-mode arguments (e.g., ["crash-mode", "native"])

        Returns:
            The test ID that can be used to find the event in Sentry
        """
        env = dict(os.environ, SENTRY_DSN=self.dsn)

        # Run with crash - capture output for test ID
        # Enable structured logs and capture a log message before crashing
        crash_args = ["log", "e2e-test", "capture-log"] + mode_args + ["crash"]
        output = run_crash_e2e(
            self.tmp_path,
            "sentry_example",
            crash_args,
            env=env,
            wait_for_daemon=True,
        )
        test_id = extract_test_id(output)

        # Print daemon logs for debugging (especially useful for Windows thread duplication investigation)
        self.print_daemon_logs()

        # Restart to send pending crash (no-setup skips scope setup but still sends)
        run(self.tmp_path, "sentry_example", ["no-setup"], env=env)

        return test_id

    def test_mode_minidump_e2e(self):
        """
        Mode 0 (MINIDUMP): Verify Sentry receives minidump-only crash.

        In minidump mode:
        - Minidump is sent as attachment
        - Server processes minidump for stacktrace
        - No client-side stackwalking
        - Multiple threads captured in minidump
        """
        test_id = self.run_crash_and_send(["crash-mode", "minidump"])

        event = poll_sentry_for_event(test_id)

        # Verify basic event structure
        assert (
            event["platform"] == "native"
        ), f"Expected platform=native, got {event.get('platform')}"

        # Get exception data (API returns it in 'entries' array)
        exception_data = get_exception_from_event(event)
        assert exception_data is not None, "Event should have exception data"
        assert "values" in exception_data, "Exception should have values"

        # Mode 0: Server processes minidump, mechanism type indicates minidump processing
        exc = exception_data["values"][0]
        assert "mechanism" in exc, "Exception should have mechanism"
        # Mechanism type will be 'minidump' when Sentry processes the minidump
        assert exc["mechanism"]["type"] in [
            "minidump",
            "signalhandler",
        ], f"Unexpected mechanism type: {exc['mechanism']['type']}"

        # Verify stacktrace (server-processed from minidump)
        assert (
            "stacktrace" in exc
        ), "Minidump mode should have stacktrace (server-processed)"
        frames = exc["stacktrace"]["frames"]
        assert (
            len(frames) >= 3
        ), f"Minidump mode should have stacktrace (>= 3 frames), got {len(frames)} frames"

        # Verify threads are captured (from minidump)
        threads_data = get_threads_from_event(event)
        assert threads_data is not None, "Minidump mode should have threads data"
        assert "values" in threads_data, "Threads should have values"
        thread_count = len(threads_data["values"])
        assert (
            thread_count >= 1
        ), f"Minidump mode should capture threads (>= 1), got {thread_count}"

        # Verify no thread duplication (regression test for Windows issue)
        verify_no_thread_duplication(threads_data, "test_mode_minidump_e2e")

    def test_mode_native_e2e(self):
        """
        Mode 1 (NATIVE): Verify Sentry receives native stacktrace, no minidump.

        In native mode:
        - Client-side stackwalking produces stacktrace
        - debug_meta with images is included
        - No minidump attachment
        - Multiple threads are captured
        """
        test_id = self.run_crash_and_send(["crash-mode", "native"])

        event = poll_sentry_for_event(test_id)

        # DEBUG: Print debug_meta and frame information for troubleshooting
        print("\n=== DEBUG: Event Structure Analysis ===")
        debug_meta = get_debug_meta_from_event(event)
        if debug_meta:
            images = debug_meta.get("images", [])
            print(f"debug_meta.images count: {len(images)}")
            for i, img in enumerate(images[:5]):  # Print first 5 images
                print(
                    f"  Image {i}: type={img.get('type')}, "
                    f"image_addr={img.get('image_addr')}, "
                    f"image_size={img.get('image_size')}, "
                    f"code_file={img.get('code_file', 'N/A')[:50]}"
                )
        else:
            print("WARNING: No debug_meta found in event!")

        exception_data = get_exception_from_event(event)
        if exception_data and "values" in exception_data:
            exc = exception_data["values"][0]
            if "stacktrace" in exc:
                frames = exc["stacktrace"]["frames"]
                print(f"Stacktrace frames count: {len(frames)}")
                for i, frame in enumerate(frames[-5:]):  # Print last 5 frames
                    print(
                        f"  Frame {len(frames)-5+i}: instruction_addr={frame.get('instructionAddr')}, "
                        f"package={frame.get('package', 'N/A')}, "
                        f"symbolicatorStatus={frame.get('symbolicatorStatus', 'N/A')}"
                    )
        print("=== END DEBUG ===\n")

        # Verify native stacktrace
        assert (
            event["platform"] == "native"
        ), f"Expected platform=native, got {event.get('platform')}"

        # Get exception data (API returns it in 'entries' array)
        exception_data = get_exception_from_event(event)
        assert exception_data is not None, "Event should have exception data"
        assert "values" in exception_data, "Exception should have values"

        exc = exception_data["values"][0]
        assert "stacktrace" in exc, "Exception should have stacktrace (native mode)"
        frames = exc["stacktrace"]["frames"]
        # Native mode should capture a meaningful stacktrace
        assert (
            len(frames) >= 3
        ), f"Native mode should have stacktrace (>= 3 frames), got {len(frames)} frames"

        # Mode 1: Mechanism should be signalhandler (not minidump)
        assert (
            exc["mechanism"]["type"] == "signalhandler"
        ), f"Native mode should use signalhandler mechanism, got {exc['mechanism']['type']}"

        # Verify threads are captured (native mode includes threads in event)
        threads_data = get_threads_from_event(event)
        assert threads_data is not None, "Native mode should have threads data"
        assert "values" in threads_data, "Threads should have values"
        thread_count = len(threads_data["values"])
        print(f"\n=== THREADS DEBUG (Native Mode) ===")
        print(f"Total thread count: {thread_count}")
        for i, t in enumerate(threads_data["values"]):
            print(
                f"  Thread {i}: id={t.get('id')}, "
                f"crashed={t.get('crashed')}, "
                f"current={t.get('current')}, "
                f"name={t.get('name', 'N/A')}"
            )
        print("=== END THREADS DEBUG ===\n")
        assert (
            thread_count >= 1
        ), f"Native mode should capture threads (>= 1), got {thread_count}"

        # Verify no thread duplication (regression test for Windows issue)
        verify_no_thread_duplication(threads_data, "test_mode_native_e2e")

    def test_mode_native_with_minidump_e2e(self):
        """
        Mode 2 (NATIVE_WITH_MINIDUMP): Verify Sentry receives both native stacktrace AND minidump.

        In native-with-minidump mode (default):
        - Client-side stackwalking produces stacktrace
        - debug_meta with images is included
        - Minidump is also attached for additional debugging
        - Multiple threads are captured (from minidump)
        """
        test_id = self.run_crash_and_send(["crash-mode", "native-with-minidump"])

        event = poll_sentry_for_event(test_id)

        # Verify native stacktrace
        assert (
            event["platform"] == "native"
        ), f"Expected platform=native, got {event.get('platform')}"

        # Get exception data (API returns it in 'entries' array)
        exception_data = get_exception_from_event(event)
        assert exception_data is not None, "Event should have exception data"
        assert "values" in exception_data, "Exception should have values"

        exc = exception_data["values"][0]
        assert (
            "stacktrace" in exc
        ), "Exception should have stacktrace (native-with-minidump mode)"
        frames = exc["stacktrace"]["frames"]
        # Native-with-minidump mode should capture a meaningful stacktrace
        assert (
            len(frames) >= 3
        ), f"Native-with-minidump mode should have stacktrace (>= 3 frames), got {len(frames)} frames"

        # Mode 2: Mechanism can be either signalhandler or minidump
        # (Sentry may process the attached minidump and use that mechanism)
        assert exc["mechanism"]["type"] in [
            "signalhandler",
            "minidump",
        ], f"Unexpected mechanism type: {exc['mechanism']['type']}"

        # Verify threads are captured (from minidump in this mode)
        threads_data = get_threads_from_event(event)
        assert (
            threads_data is not None
        ), "Native-with-minidump mode should have threads data"
        assert "values" in threads_data, "Threads should have values"
        thread_count = len(threads_data["values"])
        assert (
            thread_count >= 1
        ), f"Native-with-minidump mode should capture threads (>= 1), got {thread_count}"

        # Verify no thread duplication (regression test for Windows issue)
        verify_no_thread_duplication(threads_data, "test_mode_native_with_minidump_e2e")

    def test_default_mode_is_native_with_minidump_e2e(self):
        """
        Verify that not specifying a mode uses NATIVE_WITH_MINIDUMP (the default).

        This ensures backward compatibility - the default mode should be the
        most feature-rich option with full stacktrace and multiple threads.
        """
        test_id = self.run_crash_and_send([])  # No crash-mode argument

        event = poll_sentry_for_event(test_id)

        # Default should behave like native-with-minidump
        assert event["platform"] == "native"

        # Get exception data (API returns it in 'entries' array)
        exception_data = get_exception_from_event(event)
        assert exception_data is not None, "Event should have exception data"
        assert "values" in exception_data, "Exception should have values"

        exc = exception_data["values"][0]
        assert "stacktrace" in exc, "Default mode should have native stacktrace"
        frames = exc["stacktrace"]["frames"]
        # Default mode should capture a meaningful stacktrace
        assert (
            len(frames) >= 3
        ), f"Default mode should have stacktrace (>= 3 frames), got {len(frames)} frames"

        # Verify threads are captured (from minidump in default mode)
        threads_data = get_threads_from_event(event)
        assert threads_data is not None, "Default mode should have threads data"
        assert "values" in threads_data, "Threads should have values"
        thread_count = len(threads_data["values"])
        assert (
            thread_count >= 1
        ), f"Default mode should capture threads (>= 1), got {thread_count}"

        # Verify no thread duplication (regression test for Windows issue)
        verify_no_thread_duplication(
            threads_data, "test_default_mode_is_native_with_minidump_e2e"
        )


def test_e2e_inproc(cmake):
    """Verify that inproc can send a signal-handler crash event to Sentry."""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "inproc"})
    env = dict(os.environ, SENTRY_DSN=os.environ["SENTRY_E2E_DSN"])

    output = run_crash_e2e(
        tmp_path,
        "sentry_example",
        ["log", "e2e-test", "capture-log", "crash"],
        env=env,
    )
    test_id = extract_test_id(output)

    time.sleep(2)
    run(tmp_path, "sentry_example", ["no-setup"], env=env)

    event = poll_sentry_for_event(test_id)

    assert (
        event["platform"] == "native"
    ), f"Expected platform=native, got {event.get('platform')}"

    exception_data = get_exception_from_event(event)
    assert exception_data is not None, "Event should have exception data"
    assert "values" in exception_data, "Exception should have values"

    exc = exception_data["values"][0]
    assert (
        exc["mechanism"]["type"] == "signalhandler"
    ), f"Inproc should use signalhandler mechanism, got {exc['mechanism']['type']}"
    assert exc["mechanism"]["handled"] is False
    assert "stacktrace" in exc, "Inproc crash should include a stacktrace"
    assert "frames" in exc["stacktrace"], "Inproc stacktrace should have frames"


@pytest.mark.skipif(
    not has_breakpad,
    reason="breakpad backend not available",
)
def test_e2e_breakpad(cmake):
    """Verify that breakpad can send a minidump crash event to Sentry."""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "breakpad"})
    env = dict(os.environ, SENTRY_DSN=os.environ["SENTRY_E2E_DSN"])

    output = run_crash_e2e(
        tmp_path,
        "sentry_example",
        ["log", "e2e-test", "capture-log", "crash"],
        env=env,
    )
    test_id = extract_test_id(output)

    time.sleep(2)
    run(tmp_path, "sentry_example", ["no-setup"], env=env)

    event = poll_sentry_for_event(test_id)

    assert (
        event["platform"] == "native"
    ), f"Expected platform=native, got {event.get('platform')}"

    exception_data = get_exception_from_event(event)
    assert exception_data is not None, "Event should have exception data"
    assert "values" in exception_data, "Exception should have values"

    exc = exception_data["values"][0]
    assert exc["mechanism"]["type"] in [
        "minidump",
        "signalhandler",
    ], f"Unexpected mechanism type: {exc['mechanism']['type']}"

    assert "stacktrace" in exc, "Breakpad crash should include a stacktrace"
    frames = exc["stacktrace"]["frames"]
    assert (
        len(frames) >= 3
    ), f"Breakpad crash should have stacktrace (>= 3 frames), got {len(frames)}"

    threads_data = get_threads_from_event(event)
    assert threads_data is not None, "Breakpad crash should have threads data"
    assert "values" in threads_data, "Threads should have values"
    thread_count = len(threads_data["values"])
    assert (
        thread_count >= 1
    ), f"Breakpad crash should capture threads (>= 1), got {thread_count}"

    verify_no_thread_duplication(threads_data, "test_e2e_breakpad")


@pytest.mark.skipif(
    not has_crashpad,
    reason="crashpad backend not available",
)
def test_e2e_crashpad(cmake):
    """Verify that crashpad can send a minidump crash event to Sentry."""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "crashpad"})
    env = dict(os.environ, SENTRY_DSN=os.environ["SENTRY_E2E_DSN"])

    output = run_crash_e2e(
        tmp_path,
        "sentry_example",
        ["log", "e2e-test", "capture-log", "crashpad-wait-for-upload", "crash"],
        env=env,
    )
    test_id = extract_test_id(output)

    event = poll_sentry_for_event(test_id)

    assert (
        event["platform"] == "native"
    ), f"Expected platform=native, got {event.get('platform')}"

    exception_data = get_exception_from_event(event)
    assert exception_data is not None, "Event should have exception data"
    assert "values" in exception_data, "Exception should have values"

    exc = exception_data["values"][0]
    assert exc["mechanism"]["type"] in [
        "minidump",
        "signalhandler",
    ], f"Unexpected mechanism type: {exc['mechanism']['type']}"

    assert "stacktrace" in exc, "Crashpad crash should include a stacktrace"
    frames = exc["stacktrace"]["frames"]
    assert (
        len(frames) >= 3
    ), f"Crashpad crash should have stacktrace (>= 3 frames), got {len(frames)}"

    threads_data = get_threads_from_event(event)
    assert threads_data is not None, "Crashpad crash should have threads data"
    assert "values" in threads_data, "Threads should have values"
    thread_count = len(threads_data["values"])
    assert (
        thread_count >= 1
    ), f"Crashpad crash should capture threads (>= 1), got {thread_count}"

    verify_no_thread_duplication(threads_data, "test_e2e_crashpad")
