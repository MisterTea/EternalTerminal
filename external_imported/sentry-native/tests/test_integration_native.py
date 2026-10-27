"""
Integration tests for the native crash backend.

Tests crash handling, minidump generation, Build ID/UUID extraction,
multi-thread capture, and FPU/SIMD register capture on all platforms.
"""

import os
import subprocess
import sys
import time
import struct

import pytest

from . import (
    is_feedback_envelope,
    is_replay_envelope,
    make_dsn,
    run,
    stage_replay,
    Envelope,
    split_log_request_cond,
    REPLAY_ID,
)
from .assertions import (
    assert_breadcrumb,
    assert_debug_meta_images_do_not_overlap,
    assert_meta,
    assert_native_crash,
    assert_replay_envelope,
    assert_session,
    is_valid_hex,
    wait_for_file,
    assert_user_feedback,
)
from .conditions import has_native, has_oom, is_asan, is_tsan, is_qemu

pytestmark = pytest.mark.skipif(
    not has_native or is_qemu,
    reason="Tests need the native backend enabled",
)

# Sanitizer builds are slower, so selected native crash tests use the same 10s
# timeout the native daemon used before it respected the SDK shutdown timeout.
SANITIZER_ARGS = ["shutdown-timeout", "10000"] if is_asan or is_tsan else []


def run_crash(tmp_path, exe, args, env, **kwargs):
    """
    Run a crash test.

    When running under ASAN, we configure it to not intercept crash signals
    so that our native crash handler can run and capture the crash.
    """
    # When running under ASAN, disable ASAN's signal handling so our crash
    # handler can run. ASAN would otherwise intercept SIGSEGV/SIGABRT/etc
    # and terminate the process before our handler completes.
    if is_asan:
        # Preserve existing ASAN_OPTIONS and add signal handling overrides
        asan_opts = env.get("ASAN_OPTIONS", "")
        # Disable handling of crash signals so our handler can run
        asan_signal_opts = (
            "handle_segv=0:handle_sigbus=0:handle_abort=0:"
            "handle_sigfpe=0:handle_sigill=0:allow_user_segv_handler=1"
        )
        if asan_opts:
            env["ASAN_OPTIONS"] = f"{asan_opts}:{asan_signal_opts}"
        else:
            env["ASAN_OPTIONS"] = asan_signal_opts

    run(tmp_path, exe, args, expect_failure=True, env=env, **kwargs)


def test_native_capture_crash(cmake, httpserver):
    """Test basic crash capture with native backend"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "test-logger", "crash"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    assert len(httpserver.log) >= 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())
    assert_native_crash(envelope)


@pytest.mark.skipif(
    sys.platform != "win32" or bool(os.environ.get("TEST_MINGW")),
    reason="WER crash tests are only available in MSVC Windows builds",
)
@pytest.mark.with_wer
@pytest.mark.parametrize(
    "crash_arg,exception_code",
    [
        pytest.param("fastfail", 0xC0000602, id="fastfail"),
        pytest.param("stack-buffer-overrun", 0xC0000409, id="stack-buffer-overrun"),
    ],
)
def test_native_wer_crash(cmake, httpserver, crash_arg, exception_code):
    """Test WER crash capture with native backend"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", crash_arg],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    assert len(httpserver.log) >= 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())
    assert_native_crash(envelope, exception_code=exception_code)


@pytest.mark.skipif(not has_oom, reason="OOM test unreliable in this environment")
def test_native_oom(cmake, httpserver):
    """Test OOM crash capture with native backend"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "oom"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result


def test_native_capture_minidump_generated(cmake, httpserver):
    """Test that minidump file is generated"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    # Crash the app - we verify crash by checking minidump generation below
    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "test-logger", "crash"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    # Check for minidump file in database directory
    db_dir = tmp_path / ".sentry-native"
    assert db_dir.exists()

    assert wait_for_file(db_dir / "*.dmp"), "Minidump file should be generated"
    minidump_files = list(db_dir.glob("*.dmp"))

    # Verify minidump has correct header
    minidump_path = minidump_files[0]
    with open(minidump_path, "rb") as f:
        # Read minidump signature (should be MDMP = 0x504d444d)
        signature = struct.unpack("<I", f.read(4))[0]
        assert signature == 0x504D444D, "Minidump should have correct signature"

        # Read version
        version = struct.unpack("<I", f.read(4))[0]
        # On Windows, MiniDumpWriteDump uses system version format (0xa0f4a793)
        # On Unix, we use custom format with version 0xa793
        if sys.platform != "win32":
            assert version == 0xA793, "Minidump should have correct version"
        else:
            # Windows minidumps have a different version format
            # Just verify it's non-zero
            assert version != 0, "Minidump should have non-zero version"


# Both daemon envelope writers merge the breadcrumb ring files: the native
# stacktrace writer builds the event from scratch, while the minidump-only
# writer re-parses the parent's event JSON, merges, and re-serializes. Exercise
# both so the minidump path's extra parse/serialize roundtrip is covered too.
BREADCRUMB_CRASH_MODES = ["native", "minidump"]


@pytest.mark.parametrize("crash_mode", BREADCRUMB_CRASH_MODES)
def test_native_breadcrumbs(cmake, httpserver, crash_mode):
    """Test that breadcrumbs survive the daemon's ring-file merge.

    The crashing process appends breadcrumbs as msgpack to the ring files; the
    daemon reads, merges, and attaches them to the event. Asserting on the
    default `debug crumb` verifies that whole roundtrip, not just that an event
    arrived.
    """
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    # The default setup block adds the `debug crumb`; crash so the daemon emits
    # the event (use stdout for initialization delay under sanitizers).
    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "crash-mode", crash_mode, "crash"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    # Verify breadcrumbs in envelope
    assert len(httpserver.log) >= 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())
    assert_breadcrumb(envelope)


@pytest.mark.parametrize("crash_mode", BREADCRUMB_CRASH_MODES)
def test_native_overflow_breadcrumbs(cmake, httpserver, crash_mode):
    """Test that the daemon caps merged breadcrumbs at max_breadcrumbs.

    The example adds 3 default crumbs plus 101 numbered crumbs ("0".."100").
    With the default max_breadcrumbs (100), the daemon keeps the newest 100,
    so the count is capped and the most-recent crumb ("100") is retained.
    """
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            [
                "log",
                "stdout",
                "overflow-breadcrumbs",
                "crash-mode",
                crash_mode,
                "crash",
            ],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    assert len(httpserver.log) >= 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())
    breadcrumbs = envelope.get_event()["breadcrumbs"]

    assert len(breadcrumbs) == 100
    assert any(b.get("message") == "100" for b in breadcrumbs)


def test_native_session_tracking(cmake, httpserver):
    """Test that sessions are tracked correctly with crashes"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    # Start session and crash (use stdout to add initialization delay for TSAN)
    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "start-session", "crash"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    # Check for session data - sent as a separate envelope from the run folder
    has_session = False
    for req in httpserver.log:
        data = req[0].get_data()
        if b'"type":"session"' in data or b'"status":"crashed"' in data:
            has_session = True
            break

    assert has_session, "Should have session data (standalone or embedded)"


def test_native_signal_handling(cmake, httpserver):
    """Test that different signals are handled correctly"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    # Test SIGSEGV (use stdout to add initialization delay for TSAN)
    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "crash"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX signals only")
def test_native_sigabrt(cmake, httpserver):
    """Test SIGABRT handling"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    # Trigger SIGABRT via assert (use stdout for initialization delay under TSAN)
    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "assert"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result


def test_native_abort(cmake, httpserver):
    """Test abort() handling with native backend"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "abort"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result


def test_native_multiple_crashes(cmake, httpserver):
    """Test handling multiple crashes in sequence"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    # Crash multiple times (use stdout for initialization delay under TSAN)
    with httpserver.wait(timeout=10) as waiting:
        for i in range(3):
            run_crash(
                tmp_path,
                "sentry_example",
                ["log", "stdout", "crash"] + SANITIZER_ARGS,
                env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
            )
    assert waiting.result


def test_native_context_capture(cmake, httpserver):
    """Test that scope and context are captured"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    # Set context then crash (use log and stdout for initialization delay under TSAN)
    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "add-stacktrace", "crash"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result


def test_native_daemon_respawn(cmake, httpserver):
    """Test that daemon respawns if it dies"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    # This tests the fallback mechanism if daemon dies
    # The test is platform-specific and may need adjustment
    # Use stdout for initialization delay under TSAN
    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "crash"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result


@pytest.mark.skipif(
    sys.platform not in ["linux", "darwin"],
    reason="Multi-thread test for POSIX platforms",
)
def test_native_multithreaded_crash(cmake, httpserver):
    """Test crash from non-main thread"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    # Crash from thread (use stdout for initialization delay under TSAN)
    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "crash"] + SANITIZER_ARGS,
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result


@pytest.mark.skipif(
    sys.platform != "darwin",
    reason="Exercises the macOS thread_get_state register capture path",
)
def test_native_noncrashing_thread_unwind(cmake, httpserver):
    """Test that non-crashing threads capture unwindable stacktraces"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "crash"] + SANITIZER_ARGS,
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    assert len(httpserver.log) >= 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())
    event = envelope.get_event()

    noncrashing = [t for t in event["threads"]["values"] if not t.get("crashed")]
    assert noncrashing

    frame_counts = []
    for thread in noncrashing:
        stacktrace = thread.get("stacktrace")
        if not stacktrace:
            continue
        frames = stacktrace["frames"]
        assert all(is_valid_hex(frame["instruction_addr"]) for frame in frames)
        frame_counts.append(len(frames))

    # A buggy register capture leaves each thread with only its top frame.
    assert frame_counts and max(frame_counts) >= 2


def _parse_minidump(path):
    """
    Lightweight minidump parser. Returns a dict with:
        ``streams``: {stream_id: (size, rva)} indexed by `MINIDUMP_STREAM_TYPE`
        ``data``: raw bytes
        ``crashing_thread_id``: from the Exception stream when present
        ``threads``: list of dicts: thread_id, stack_start, stack_size, sp (from CONTEXT)
        ``memory_regions``: list of (start, size) from MemoryListStream

    Used to verify cross-stream invariants the writer must uphold (e.g.
    each ``MINIDUMP_THREAD::stack`` descriptor must cover the SP held by
    that thread's CONTEXT). Bypasses any external minidump-parsing library
    so the tests can run on every CI runner.
    """
    with open(path, "rb") as f:
        data = f.read()

    magic, _version, stream_count, stream_dir_rva = struct.unpack_from("<IIII", data, 0)
    assert magic == 0x504D444D, f"bad minidump magic {magic:#x}"

    streams = {}
    for i in range(stream_count):
        sid, size, rva = struct.unpack_from("<III", data, stream_dir_rva + i * 12)
        streams[sid] = (size, rva)

    crashing_thread_id = None
    if 6 in streams:  # Exception stream
        _size, rva = streams[6]
        crashing_thread_id = struct.unpack_from("<I", data, rva)[0]

    # SystemInfo (id=7) tells us the CPU architecture, which decides how to
    # decode the per-thread CONTEXT block (where SP lives).
    cpu_arch = None
    if 7 in streams:
        _size, rva = streams[7]
        cpu_arch = struct.unpack_from("<H", data, rva)[0]
    # ProcessorArchitecture: 0=X86, 5=ARM, 9=AMD64, 0x8003=ARM64 (Breakpad)
    is_arm64 = cpu_arch in (0x8003, 0xC)
    is_amd64 = cpu_arch == 9
    is_x86 = cpu_arch == 0

    threads = []
    if 3 in streams:  # ThreadList
        _size, rva = streams[3]
        count = struct.unpack_from("<I", data, rva)[0]
        for i in range(count):
            base = rva + 4 + i * 48
            tid = struct.unpack_from("<I", data, base)[0]
            stack_start = struct.unpack_from("<Q", data, base + 24)[0]
            stack_size = struct.unpack_from("<I", data, base + 32)[0]
            ctx_size = struct.unpack_from("<I", data, base + 40)[0]
            ctx_rva = struct.unpack_from("<I", data, base + 44)[0]

            sp = None
            if ctx_size and ctx_rva:
                # Decode SP from the architecture-specific CONTEXT block.
                if is_arm64:
                    # Breakpad CONTEXT_ARM64: context_flags(u32) + cpsr(u32)
                    # + iregs[31]*u64 + sp(u64). SP at offset 8 + 31*8 = 256.
                    sp = struct.unpack_from("<Q", data, ctx_rva + 8 + 31 * 8)[0]
                elif is_amd64:
                    # CONTEXT_AMD64: rsp at offset 0x98.
                    sp = struct.unpack_from("<Q", data, ctx_rva + 0x98)[0]
                elif is_x86:
                    # CONTEXT_X86: esp at offset 0xC4.
                    sp = struct.unpack_from("<I", data, ctx_rva + 0xC4)[0]

            threads.append(
                {
                    "thread_id": tid,
                    "stack_start": stack_start,
                    "stack_size": stack_size,
                    "sp": sp,
                }
            )

    memory_regions = []
    if 5 in streams:  # MemoryListStream
        _size, rva = streams[5]
        count = struct.unpack_from("<I", data, rva)[0]
        for i in range(count):
            base = rva + 4 + i * 16
            start = struct.unpack_from("<Q", data, base)[0]
            region_size = struct.unpack_from("<I", data, base + 8)[0]
            memory_regions.append((start, region_size))
    elif 9 in streams:  # Memory64ListStream (full-memory dumps)
        _size, rva = streams[9]
        count = struct.unpack_from("<Q", data, rva)[0]
        # Memory64 descriptors are 16 bytes: start(8) + size(8). The bytes
        # themselves are concatenated starting at a base RVA stored after
        # the count; we don't need them for our assertions.
        for i in range(count):
            entry = rva + 16 + i * 16
            start = struct.unpack_from("<Q", data, entry)[0]
            region_size = struct.unpack_from("<Q", data, entry + 8)[0]
            memory_regions.append((start, region_size))

    return {
        "streams": streams,
        "data": data,
        "crashing_thread_id": crashing_thread_id,
        "threads": threads,
        "memory_regions": memory_regions,
        "cpu_arch": cpu_arch,
    }


def test_native_minidump_streams(cmake, httpserver):
    """
    Test that minidump contains required streams *and* that each
    per-thread ``MINIDUMP_THREAD::stack`` descriptor's
    ``start_of_memory_range`` falls inside the captured stack range
    that the thread's CONTEXT block records as its SP.

    This is the regression guard for an issue where the native macOS
    backend would populate every thread's stack descriptor with a
    daemon-time SP (obtained via ``thread_get_state`` long after the
    signal handler had moved the crashing thread's SP deep into
    libsystem code waiting on the daemon IPC). The crashing thread's
    ``CONTEXT`` had the right SP (taken from the signal handler's
    ucontext snapshot), but the ``stack`` descriptor pointed at
    libsystem code pages — so the two streams disagreed and downstream
    minidump tools couldn't follow the stack from SP. The fix matches
    each Mach thread back to the signal-handler-captured state by
    ``thread_id`` and uses *that* SP for the stack descriptor.

    The assertion runs on every platform — Linux and Windows already
    populated the descriptor correctly; this guards against a regression
    that breaks the cross-stream invariant.
    """
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    # Crash (use stdout for initialization delay under TSAN)
    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "crash"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    # Find minidump
    db_dir = tmp_path / ".sentry-native"
    assert wait_for_file(db_dir / "*.dmp")
    minidump_files = list(db_dir.glob("*.dmp"))
    assert len(minidump_files) > 0

    dump = _parse_minidump(minidump_files[0])

    # Required streams: ThreadList (3), ModuleList (4), SystemInfo (7).
    assert 3 in dump["streams"], "Should have ThreadList stream"
    assert 4 in dump["streams"], "Should have ModuleList stream"
    assert 7 in dump["streams"], "Should have SystemInfo stream"

    # Per-thread stack descriptor accuracy: every thread that has a CONTEXT
    # block must have a stack descriptor whose range contains the CONTEXT's
    # SP. Threads with size=0 (couldn't capture stack) are tolerated.
    for thread in dump["threads"]:
        sp = thread["sp"]
        if sp is None or sp == 0:
            continue
        if thread["stack_size"] == 0:
            continue
        stack_end = thread["stack_start"] + thread["stack_size"]
        assert thread["stack_start"] <= sp < stack_end, (
            f"thread {thread['thread_id']}: SP {sp:#x} is outside the "
            f"per-thread stack descriptor range "
            f"[{thread['stack_start']:#x}..{stack_end:#x}). "
            f"This means MINIDUMP_THREAD::stack and the thread's CONTEXT "
            f"disagree about where the stack lives, breaking any minidump "
            f"reader that walks the stack from SP."
        )

    # Exception-stream encoding: per Breakpad/Crashpad convention on macOS
    # and Linux, MINIDUMP_EXCEPTION::exception_code carries the Mach
    # exception type (macOS) or matches the Linux/Windows-style code
    # respectively — *not* the BSD signal number. Putting the signal
    # there triggers a name collision (e.g. SIGSEGV=11 equals the Mach
    # enum value for EXC_RESOURCE), causing every minidump reader that
    # respects the convention (minidump-rs, breakpad processor, the
    # crashpad tools) to decode the wrong crash reason.
    #
    # MINIDUMP_EXCEPTION_STREAM layout (verified against the Microsoft
    # Minidump SDK headers and Breakpad's md_exception.h):
    #
    #     +0   thread_id                u32
    #     +4   __alignment              u32
    #     +8   exception_code           u32  ← Mach exception type on macOS
    #     +12  exception_flags          u32  ← Mach exception subtype
    #     +16  exception_record         u64  (pointer field, always zero)
    #     +24  exception_address        u64
    #     +32  number_parameters        u32
    #     +36  __unusedAlignment        u32
    #     +40  exception_information[0] u64
    #     +48  exception_information[1] u64
    #     ...
    if 6 in dump["streams"]:
        size, rva = dump["streams"][6]
        data = dump["data"]
        exception_code = struct.unpack_from("<I", data, rva + 8)[0]
        if sys.platform == "darwin":
            # Mach exception types: EXC_BAD_ACCESS=1, EXC_BAD_INSTRUCTION=2,
            # EXC_ARITHMETIC=3, EXC_SOFTWARE=5, EXC_BREAKPOINT=6, EXC_CRASH=10
            # (full enum runs through 13). The bug to guard against is
            # writing the BSD signal (11 = SIGSEGV) directly, which lands
            # on EXC_RESOURCE — a totally different concept.
            valid_mach_types = {1, 2, 3, 5, 6, 10}
            assert exception_code in valid_mach_types, (
                f"exception_code = {exception_code} is not a valid Mach "
                f"exception type for crash-derived dumps. Per "
                f"Breakpad/Crashpad convention, this field must carry the "
                f"Mach exception type (EXC_BAD_ACCESS, EXC_CRASH, etc.), "
                f"not the BSD signal number — otherwise minidump readers "
                f"misdecode the crash reason. See "
                f"src/backends/native/minidump/sentry_minidump_macos.c "
                f"::bsd_signal_to_mach_exception."
            )
            # exception_information[0] should carry the BSD signal so
            # consumers that want it can still find it.
            number_parameters = struct.unpack_from("<I", data, rva + 32)[0]
            assert number_parameters >= 1, (
                f"number_parameters = {number_parameters} — the writer "
                f"should populate exception_information[0] with the BSD "
                f"signal so consumers that key on it (lldb, custom "
                f"analyzers) don't lose access to it."
            )
            bsd_signum = struct.unpack_from("<Q", data, rva + 40)[0]
            # Common crash signals: SIGILL=4, SIGTRAP=5, SIGABRT=6,
            # SIGFPE=8, SIGBUS=10, SIGSEGV=11, SIGSYS=12.
            assert bsd_signum in {4, 5, 6, 8, 10, 11, 12}, (
                f"exception_information[0] = {bsd_signum} doesn't look "
                f"like a BSD signal number for a typical crash. The "
                f"writer should put the originating signal there for "
                f"consumers that key on it."
            )


def _codesign_for_task_for_pid(*paths):
    """
    Ad-hoc codesign macOS binaries with ``com.apple.security.cs.debugger``
    + ``com.apple.security.get-task-allow`` + hardened runtime so the
    sentry-crash daemon can call ``task_for_pid`` on the parent process.
    Without this, sentry-native's SMART minidump mode falls back to a
    minimal capture (module headers only — no heap-region scan) because
    ``mach_vm_region`` requires the task port to enumerate writable VM
    mappings.

    Real Apple Developer certificates are NOT needed; ad-hoc signing
    (``codesign --sign -``) is sufficient as long as the binaries get
    consistent entitlements.
    """
    import subprocess
    import tempfile
    import textwrap

    ent_xml = textwrap.dedent("""\
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
            <key>com.apple.security.cs.debugger</key>
            <true/>
            <key>com.apple.security.get-task-allow</key>
            <true/>
        </dict>
        </plist>
        """)
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".entitlements", delete=False
    ) as f:
        f.write(ent_xml)
        ent_path = f.name

    for path in paths:
        if not os.path.exists(path):
            continue
        subprocess.run(
            [
                "codesign",
                "--force",
                "--sign",
                "-",
                "--options",
                "runtime",
                "--entitlements",
                ent_path,
                path,
            ],
            check=True,
            capture_output=True,
        )


def test_native_smart_mode_captures_indirect_heap_memory(cmake, httpserver):
    """
    Verify SMART minidump mode captures memory regions referenced by the
    crashing thread's registers + stack contents — not just module
    headers + thread stacks.

    The published behaviour of ``SENTRY_MINIDUMP_MODE_SMART`` (the
    default) is "stack-only dump plus ~1 KiB around every writable-heap
    pointer reachable from the crashing thread's registers or stack
    words, capped at 4 MiB". All three backends now wire this through:

      * macOS / Linux: the native backend scans the captured stack
        words + register file in the daemon, looks up each candidate
        pointer against ``mach_vm_region`` / ``/proc/self/maps``, and
        appends a chunk around each writable-heap hit to
        MemoryListStream.
      * Windows: the native backend passes
        ``MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs``
        to ``MiniDumpWriteDump`` so the OS does the equivalent scan.

    The minimal-mode fallback on macOS (when ``task_for_pid`` is
    blocked by the sandbox) silently breaks the SMART-mode contract by
    emitting *only* module header pages. This test ad-hoc codesigns the
    example + daemon on macOS so ``task_for_pid`` succeeds (no Apple
    Developer cert needed; ad-hoc signing is enough), then asserts
    MemoryListStream contains at least one region in a typical heap
    address band (i.e. *not* inside any loaded-module image range and
    *not* inside any captured thread stack). On Linux ``/proc/self/maps``
    is readable in-process so no signing is needed; on Windows
    ``MiniDumpWriteDump`` runs as the same user as the crashing process
    and likewise needs no extra setup.
    """
    # Static build so the hardened-runtime process can load itself without
    # tripping the dyld "different team IDs" check on ad-hoc-signed dylibs.
    tmp_path = cmake(
        ["sentry_example"],
        {"SENTRY_BACKEND": "native", "BUILD_SHARED_LIBS": "OFF"},
    )

    if sys.platform == "darwin":
        _codesign_for_task_for_pid(
            str(tmp_path / "sentry_example"),
            str(tmp_path / "sentry-crash"),
        )

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=15) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "crash"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    db_dir = tmp_path / ".sentry-native"
    assert wait_for_file(db_dir / "*.dmp")
    minidump_files = list(db_dir.glob("*.dmp"))
    assert len(minidump_files) > 0

    dump = _parse_minidump(minidump_files[0])

    # Build a quick set of "module image" address ranges so we can spot
    # captured regions that are *not* in any module (heap candidates).
    module_ranges = []
    if 4 in dump["streams"]:
        size, rva = dump["streams"][4]
        data = dump["data"]
        module_count = struct.unpack_from("<I", data, rva)[0]
        # MINIDUMP_MODULE is 108 bytes on disk
        for i in range(module_count):
            entry = rva + 4 + i * 108
            base = struct.unpack_from("<Q", data, entry)[0]
            size = struct.unpack_from("<I", data, entry + 8)[0]
            module_ranges.append((base, base + size))

    # Stack ranges from per-thread descriptors.
    stack_ranges = [
        (t["stack_start"], t["stack_start"] + t["stack_size"])
        for t in dump["threads"]
        if t["stack_size"] > 0
    ]

    def in_any(addr, ranges):
        return any(lo <= addr < hi for lo, hi in ranges)

    heap_candidates = [
        (start, size)
        for (start, size) in dump["memory_regions"]
        if not in_any(start, module_ranges) and not in_any(start, stack_ranges)
    ]

    assert dump[
        "memory_regions"
    ], "SMART mode must populate MemoryListStream with at least one region"
    assert heap_candidates, (
        f"SMART mode must capture at least one region outside the loaded "
        f"module image ranges and outside the per-thread stack ranges. "
        f"All {len(dump['memory_regions'])} captured regions landed in "
        f"either module or stack ranges, which means the indirect-memory "
        f"scan didn't fire (typically because task_for_pid failed on "
        f"macOS without cs.debugger / get-task-allow entitlements)."
    )


def test_native_cleanup(cmake):
    """Test that cleanup works properly"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    # Run and exit cleanly
    run(
        tmp_path,
        "sentry_example",
        ["log", "no-setup"],
    )

    # Database should exist
    db_dir = tmp_path / ".sentry-native"
    assert db_dir.exists()


def test_native_no_dsn_no_crash(cmake):
    """Test that without DSN, crashes don't create files"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    # Run without DSN (use stdout for initialization delay under TSAN)
    run_crash(
        tmp_path,
        "sentry_example",
        ["log", "stdout", "crash"],
        env=dict(os.environ, SENTRY_DSN=""),
    )

    # Should not create database
    db_dir = tmp_path / ".sentry-native"
    if db_dir.exists():
        minidump_files = list(db_dir.glob("*.dmp"))
        # Minidumps might still be generated for debugging
        # but won't be uploaded


def test_native_external_crash_reporter(cmake, httpserver):
    """Test external crash reporter invocation with native backend"""
    tmp_path = cmake(
        ["sentry_example", "sentry_crash_reporter"], {"SENTRY_BACKEND": "native"}
    )
    cache_dir = tmp_path.joinpath(".sentry-native/cache")

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    # Crash and use external reporter
    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "crash-reporter", "cache-keep", "crash"],
            env=env,
        )
    assert waiting.result

    # Should have sent crash report and feedback via external reporter
    assert len(httpserver.log) == 2
    feedback_request, crash_request = split_log_request_cond(
        httpserver.log, is_feedback_envelope
    )
    feedback = feedback_request.get_data()
    crash = crash_request.get_data()

    # Verify it's a minidump crash report and user feedback
    envelope = Envelope.deserialize(crash)
    assert envelope.headers["cache_dir"] == str(cache_dir)
    assert_meta(envelope, integration="native")
    assert_breadcrumb(envelope)

    envelope = Envelope.deserialize(feedback)
    assert_user_feedback(envelope)


def test_crash_mode_minidump_only(cmake, httpserver):
    """Mode 1: Should produce envelope with minidump attachment only"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    # Crash with mode 1 (minidump only)
    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "crash-mode", "minidump", "crash"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    assert len(httpserver.log) >= 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())

    # Should have minidump attachment
    has_minidump = any(
        item.headers.get("type") == "attachment"
        and item.headers.get("attachment_type") == "event.minidump"
        for item in envelope.items
    )
    assert has_minidump, "Minidump mode should include minidump"


def test_crash_mode_native_only(cmake, httpserver):
    """Mode 2: Should produce envelope with native stacktrace, no minidump"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    # Crash with mode 2 (native only)
    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "crash-mode", "native", "crash"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    assert len(httpserver.log) >= 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())

    # Should NOT have minidump
    has_minidump = any(
        item.headers.get("type") == "attachment"
        and item.headers.get("attachment_type") == "event.minidump"
        for item in envelope.items
    )
    assert not has_minidump, "Native mode should NOT include minidump"

    # Should have native stacktrace
    event = envelope.get_event()
    assert event is not None
    assert "exception" in event
    exc = event["exception"]["values"][0]
    assert exc["mechanism"]["type"] == "signalhandler"
    assert "stacktrace" in exc
    assert len(exc["stacktrace"]["frames"]) > 0

    # Each frame should have instruction_addr
    for frame in exc["stacktrace"]["frames"]:
        assert "instruction_addr" in frame

    if sys.platform == "win32" or sys.platform == "linux":
        # At least some frames should have symbolicated function names
        assert any(
            frame.get("function") is not None for frame in exc["stacktrace"]["frames"]
        )

    # Should have debug_meta
    assert "debug_meta" in event
    assert len(event["debug_meta"]["images"]) > 0
    if sys.platform == "darwin":
        assert_debug_meta_images_do_not_overlap(event)


def test_crash_mode_native_with_minidump(cmake, httpserver):
    """Mode 3 (default): Should have both native stacktrace AND minidump"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    # Default mode should be NATIVE_WITH_MINIDUMP
    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "crash"],  # No crash-mode arg = use default
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    assert len(httpserver.log) >= 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())

    # Should have BOTH minidump attachment
    has_minidump = any(
        item.headers.get("type") == "attachment"
        and item.headers.get("attachment_type") == "event.minidump"
        for item in envelope.items
    )
    assert has_minidump, "Native with minidump mode should include minidump"

    # AND native stacktrace
    event = envelope.get_event()
    assert event is not None
    assert "exception" in event
    exc = event["exception"]["values"][0]
    assert exc["mechanism"]["type"] == "signalhandler"
    assert "stacktrace" in exc
    assert len(exc["stacktrace"]["frames"]) > 0
    if sys.platform == "win32" or sys.platform == "linux":
        # At least some frames should have symbolicated function names
        assert any(
            frame.get("function") is not None for frame in exc["stacktrace"]["frames"]
        )

    # Should have debug_meta
    assert "debug_meta" in event
    if sys.platform == "darwin":
        assert_debug_meta_images_do_not_overlap(event)


def test_native_cache_consent(cmake, httpserver):
    """Daemon honors revoked consent: envelope is cached, not sent. Giving
    consent in a subsequent run flushes the cached envelope."""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})
    cache_dir = tmp_path / ".sentry-native" / "cache"
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    # 1) Crash while consent is revoked. Envelope is cached, no upload.
    run_crash(
        tmp_path,
        "sentry_example",
        [
            "log",
            "stdout",
            "cache-keep",
            "http-retry",
            "require-user-consent",
            "user-consent-revoke",
            "crash",
        ],
        env=env,
    )

    assert wait_for_file(cache_dir / "*.envelope")
    assert len(list(cache_dir.glob("*.envelope"))) == 1
    assert len(httpserver.log) == 0

    # 2) Give consent. The cached envelope should be flushed to the server.
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    with httpserver.wait(timeout=10) as waiting:
        run(
            tmp_path,
            "sentry_example",
            [
                "log",
                "cache-keep",
                "http-retry",
                "require-user-consent",
                "user-consent-give",
            ],
            env=env,
        )
    assert waiting.result
    assert len(list(cache_dir.glob("*.envelope"))) == 0


@pytest.mark.parametrize("cache_keep", [True, False])
def test_native_cache_keep(cmake, cache_keep, unreachable_dsn):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})
    db_dir = tmp_path / ".sentry-native"
    cache_dir = db_dir / "cache"
    env = dict(os.environ, SENTRY_DSN=unreachable_dsn)

    # crash -> daemon sends via HTTP -> unreachable -> cache
    run_crash(
        tmp_path,
        "sentry_example",
        ["log", "stdout", "crash"] + (["cache-keep"] if cache_keep else []),
        env=env,
        wait_for_daemon=not cache_keep,
    )

    if cache_keep:
        assert wait_for_file(cache_dir / "*.envelope")
        cache_files = list(cache_dir.glob("*.envelope"))
        assert len(cache_files) == 1
        dmp_files = list(cache_dir.glob("*.dmp"))
        assert len(dmp_files) == 1
        assert cache_files[0].stem == dmp_files[0].stem
    else:
        assert len(list(cache_dir.glob("*.envelope"))) == 0


def test_native_restart_on_crash(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        # The restarted child inherits stdio, so PIPE waits for it without a sleep.
        run_crash(
            tmp_path,
            "sentry_example",
            ["crash", "restart-on-crash"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    assert waiting.result
    assert len(httpserver.log) == 2
    for req in httpserver.log:
        envelope = Envelope.deserialize(req[0].get_data())
        assert_native_crash(envelope)


def test_native_replay_envelope(cmake, httpserver):
    """A staged replay referenced by the crash event's `contexts.replay` is
    sent as a correctly structured `replay_video` envelope by the daemon."""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    replays, video = stage_replay(tmp_path)

    # crash envelope + replay envelope
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "replay-context", "crash"] + SANITIZER_ARGS,
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
            wait_for_daemon=True,
        )
    assert waiting.result

    replay_requests = [
        req for req, _ in httpserver.log if is_replay_envelope(req.get_data())
    ]
    assert len(replay_requests) == 1, "expected exactly one replay_video envelope"
    envelope = Envelope.deserialize(replay_requests[0].get_data())
    assert_replay_envelope(envelope, video)

    # the staged replay is consumed exactly once
    assert not (replays / f"replay-{REPLAY_ID}.mp4").exists()
    assert not (replays / f"replay-{REPLAY_ID}.json").exists()


def test_native_replay_orphan_not_flushed(cmake, httpserver):
    """A staged replay that the crash event does not reference is neither
    sent nor consumed."""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    replays, _ = stage_replay(tmp_path)

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        run_crash(
            tmp_path,
            "sentry_example",
            ["log", "stdout", "crash"] + SANITIZER_ARGS,
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
            wait_for_daemon=True,
        )
    assert waiting.result

    for req, _ in httpserver.log:
        assert not is_replay_envelope(req.get_data())
    assert (replays / f"replay-{REPLAY_ID}.mp4").exists()
    assert (replays / f"replay-{REPLAY_ID}.json").exists()
