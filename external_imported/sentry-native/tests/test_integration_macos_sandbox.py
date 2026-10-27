"""
Integration tests for the native backend running inside macOS App Sandbox.

Verifies that the sandbox-safe IPC (file-backed mmap, pthread mutex,
posix_spawn) works correctly when com.apple.security.app-sandbox is enabled.
"""

import os
import sys
import plistlib
import shutil
import struct
import subprocess
import tempfile
import textwrap

import pytest

from . import make_dsn, Envelope
from .assertions import wait_for_file
from .conditions import has_native

pytestmark = pytest.mark.skipif(
    not (has_native and sys.platform == "darwin"),
    reason="macOS App Sandbox tests require native backend on macOS",
)


def _create_sandbox_app_bundle(tmp_path, exe_name="sentry_example"):
    """
    Create a minimal .app bundle with App Sandbox entitlements around
    the already-built executable and its companion sentry-crash daemon.

    Returns the path to the executable inside the bundle.
    """
    app_dir = os.path.join(str(tmp_path), "SentryTest.app")
    contents_dir = os.path.join(app_dir, "Contents")
    macos_dir = os.path.join(contents_dir, "MacOS")
    os.makedirs(macos_dir, exist_ok=True)

    # Copy executable and sentry-crash into the bundle
    src_exe = os.path.join(str(tmp_path), exe_name)
    src_daemon = os.path.join(str(tmp_path), "sentry-crash")
    dst_exe = os.path.join(macos_dir, exe_name)
    dst_daemon = os.path.join(macos_dir, "sentry-crash")

    shutil.copy2(src_exe, dst_exe)
    if os.path.exists(src_daemon):
        shutil.copy2(src_daemon, dst_daemon)

    # Copy libsentry.dylib if it exists (shared library build)
    src_lib = os.path.join(str(tmp_path), "libsentry.dylib")
    if os.path.exists(src_lib):
        dst_lib = os.path.join(macos_dir, "libsentry.dylib")
        shutil.copy2(src_lib, dst_lib)

    # Write minimal Info.plist
    info_plist = {
        "CFBundleIdentifier": "io.sentry.native.test",
        "CFBundleName": "SentryTest",
        "CFBundleExecutable": exe_name,
        "CFBundleVersion": "1.0",
        "CFBundleShortVersionString": "1.0",
        "CFBundlePackageType": "APPL",
        "LSMinimumSystemVersion": "10.15",
    }
    with open(os.path.join(contents_dir, "Info.plist"), "wb") as f:
        plistlib.dump(info_plist, f)

    # Write entitlements with App Sandbox enabled and outgoing network
    entitlements_path = os.path.join(str(tmp_path), "sandbox.entitlements")
    entitlements = {
        "com.apple.security.app-sandbox": True,
        # Allow outgoing network connections (needed for HTTP transport)
        "com.apple.security.network.client": True,
    }
    with open(entitlements_path, "wb") as f:
        plistlib.dump(entitlements, f)

    return app_dir, dst_exe, dst_daemon, entitlements_path


def _codesign_bundle(app_dir, entitlements_path, exe_name="sentry_example"):
    """
    Ad-hoc sign the .app bundle with sandbox entitlements.

    IMPORTANT: Only the main executable gets sandbox entitlements.
    Helper binaries (sentry-crash daemon, dylibs) are signed ad-hoc
    WITHOUT entitlements. If a helper binary is signed with sandbox
    entitlements but lacks a CFBundleIdentifier, macOS will abort it
    with SIGTRAP during _libsecinit_appsandbox initialization.
    """
    macos_dir = os.path.join(app_dir, "Contents", "MacOS")

    # Sign helper binaries (daemon, libraries) ad-hoc WITHOUT entitlements
    for name in os.listdir(macos_dir):
        if name == exe_name:
            continue  # Main exe gets signed with the bundle
        path = os.path.join(macos_dir, name)
        if os.access(path, os.X_OK) or name.endswith(".dylib"):
            subprocess.run(
                ["codesign", "--force", "--sign", "-", path],
                check=True,
                capture_output=True,
            )

    # Sign the bundle (which signs the main executable with entitlements)
    subprocess.run(
        [
            "codesign",
            "--force",
            "--sign",
            "-",
            "--entitlements",
            entitlements_path,
            app_dir,
        ],
        check=True,
        capture_output=True,
    )


def _run_sandboxed(app_dir, exe_name, args, env, expect_failure=False):
    """
    Run the sandboxed app bundle's executable directly.
    The sandbox is enforced by the kernel based on the code signature's
    entitlements.
    """
    exe_path = os.path.join(app_dir, "Contents", "MacOS", exe_name)

    # Set DYLD_LIBRARY_PATH so the executable can find libsentry.dylib
    # inside the bundle (dyld respects this even in sandbox for ad-hoc signed)
    run_env = dict(env)
    run_env["DYLD_LIBRARY_PATH"] = os.path.join(app_dir, "Contents", "MacOS")

    result = subprocess.run(
        [exe_path] + args,
        env=run_env,
        capture_output=True,
        timeout=30,
    )

    if expect_failure:
        assert result.returncode != 0, (
            f"Expected crash but process exited cleanly.\n"
            f"stdout: {result.stdout.decode(errors='replace')}\n"
            f"stderr: {result.stderr.decode(errors='replace')}"
        )
    else:
        assert result.returncode == 0, (
            f"Process failed unexpectedly (rc={result.returncode}).\n"
            f"stdout: {result.stdout.decode(errors='replace')}\n"
            f"stderr: {result.stderr.decode(errors='replace')}"
        )

    return result


def test_sandbox_init_succeeds(cmake):
    """
    Verify that sentry_init() succeeds inside App Sandbox.
    This is the core regression test: before the fix, sem_open and shm_open
    would fail with EACCES inside sandbox, causing init to fail.
    """
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    app_dir, exe, daemon, ent = _create_sandbox_app_bundle(tmp_path)
    _codesign_bundle(app_dir, ent)

    # Run with no-setup to test init/shutdown without crashing
    result = _run_sandboxed(
        app_dir,
        "sentry_example",
        ["log", "no-setup"],
        env=dict(os.environ),
        expect_failure=False,
    )

    # Should not contain IPC failure messages
    stderr = result.stderr.decode(errors="replace")
    assert "failed to open shared memory" not in stderr.lower()
    assert "failed to create ipc semaphore" not in stderr.lower()
    assert "posix_spawn failed" not in stderr.lower()


def test_sandbox_crash_capture(cmake, httpserver):
    """
    Full end-to-end: crash inside sandbox -> daemon captures minidump ->
    sends envelope to HTTP server.
    """
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    app_dir, exe, daemon, ent = _create_sandbox_app_bundle(tmp_path)
    _codesign_bundle(app_dir, ent)

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=15) as waiting:
        _run_sandboxed(
            app_dir,
            "sentry_example",
            ["log", "stdout", "crash"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
            expect_failure=True,
        )

    assert waiting.result, "Crash envelope should be received from sandboxed app"


def test_sandbox_minidump_generated(cmake, httpserver):
    """
    Verify that a valid minidump is generated inside sandbox.
    """
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    app_dir, exe, daemon, ent = _create_sandbox_app_bundle(tmp_path)
    _codesign_bundle(app_dir, ent)

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=15) as waiting:
        _run_sandboxed(
            app_dir,
            "sentry_example",
            ["log", "stdout", "crash"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
            expect_failure=True,
        )

    assert waiting.result

    # Verify envelope contains minidump
    assert len(httpserver.log) >= 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())

    has_minidump = any(
        item.headers.get("type") == "attachment"
        and item.headers.get("attachment_type") == "event.minidump"
        for item in envelope.items
    )
    assert has_minidump, "Sandboxed crash should produce minidump"

    # Verify minidump has correct signature
    for item in envelope.items:
        if (
            item.headers.get("type") == "attachment"
            and item.headers.get("attachment_type") == "event.minidump"
        ):
            signature = struct.unpack("<I", item.payload.bytes[:4])[0]
            assert signature == 0x504D444D, "Minidump should have MDMP signature"
            break


def test_sandbox_native_stacktrace(cmake, httpserver):
    """
    Verify native stacktrace mode works in sandbox (no minidump, just
    symbolicated frames).
    """
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    app_dir, exe, daemon, ent = _create_sandbox_app_bundle(tmp_path)
    _codesign_bundle(app_dir, ent)

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=15) as waiting:
        _run_sandboxed(
            app_dir,
            "sentry_example",
            ["log", "stdout", "crash-mode", "native", "crash"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
            expect_failure=True,
        )

    assert waiting.result

    assert len(httpserver.log) >= 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())

    # Should have native stacktrace, not minidump
    has_minidump = any(
        item.headers.get("type") == "attachment"
        and item.headers.get("attachment_type") == "event.minidump"
        for item in envelope.items
    )
    assert not has_minidump, "Native-only mode should not include minidump"

    event = envelope.get_event()
    assert event is not None, "Should have event"
    assert "exception" in event
    exc = event["exception"]["values"][0]
    assert exc["mechanism"]["type"] == "signalhandler"
    assert "stacktrace" in exc
    assert len(exc["stacktrace"]["frames"]) > 0
