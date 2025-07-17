import os
import shutil
import subprocess
import sys
import time

import pytest

from . import (
    make_dsn,
    run,
    Envelope,
)
from .proxy import (
    setup_proxy_env_vars,
    cleanup_proxy_env_vars,
    start_mitmdump,
    proxy_test_finally,
)
from .assertions import (
    assert_crashpad_upload,
    assert_session,
    assert_gzip_file_header,
)
from .conditions import has_crashpad

pytestmark = pytest.mark.skipif(not has_crashpad, reason="tests need crashpad backend")

# Windows and Linux are currently able to flush all the state on crash
flushes_state = sys.platform != "darwin"


def test_crashpad_capture(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "crashpad"})

    # make sure we are isolated from previous runs
    shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")

    run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "capture-event"],
        check=True,
        env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
    )

    assert len(httpserver.log) == 2


def _setup_crashpad_proxy_test(cmake, httpserver, proxy):
    proxy_process = start_mitmdump(proxy) if proxy else None

    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "crashpad"})

    # make sure we are isolated from previous runs
    shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver, proxy_host=True))
    httpserver.expect_oneshot_request("/api/123456/minidump/").respond_with_data("OK")

    return env, proxy_process, tmp_path


def test_crashpad_crash_proxy_env(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    setup_proxy_env_vars(port=8080)
    try:
        env, proxy_process, tmp_path = _setup_crashpad_proxy_test(
            cmake, httpserver, "http-proxy"
        )

        with httpserver.wait(timeout=10) as waiting:
            child = run(tmp_path, "sentry_example", ["log", "crash"], env=env)
            assert child.returncode  # well, it's a crash after all
        assert waiting.result
    finally:
        cleanup_proxy_env_vars()
        proxy_test_finally(1, httpserver, proxy_process)


def test_crashpad_crash_proxy_env_port_incorrect(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    setup_proxy_env_vars(port=8081)
    try:
        env, proxy_process, tmp_path = _setup_crashpad_proxy_test(
            cmake, httpserver, "http-proxy"
        )

        with pytest.raises(AssertionError):
            with httpserver.wait(timeout=10):
                child = run(tmp_path, "sentry_example", ["log", "crash"], env=env)
                assert child.returncode  # well, it's a crash after all
    finally:
        cleanup_proxy_env_vars()
        proxy_test_finally(0, httpserver, proxy_process)


def test_crashpad_proxy_set_empty(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    setup_proxy_env_vars(port=8080)  # we start the proxy but expect it to remain unused
    try:
        env, proxy_process, tmp_path = _setup_crashpad_proxy_test(
            cmake, httpserver, "http-proxy"
        )

        with httpserver.wait(timeout=10) as waiting:
            child = run(
                tmp_path, "sentry_example", ["log", "crash", "proxy-empty"], env=env
            )
            assert child.returncode  # well, it's a crash after all
        assert waiting.result

    finally:
        cleanup_proxy_env_vars()
        proxy_test_finally(1, httpserver, proxy_process, expected_proxy_logsize=0)


def test_crashpad_proxy_https_not_http(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    # we start the proxy but expect it to remain unused (dsn is http, so shouldn't use https proxy)
    os.environ["https_proxy"] = f"http://localhost:8080"
    try:
        env, proxy_process, tmp_path = _setup_crashpad_proxy_test(
            cmake, httpserver, "http-proxy"
        )

        with httpserver.wait(timeout=10) as waiting:
            child = run(tmp_path, "sentry_example", ["log", "crash"], env=env)
            assert child.returncode  # well, it's a crash after all
        assert waiting.result

    finally:
        del os.environ["https_proxy"]
        proxy_test_finally(1, httpserver, proxy_process, expected_proxy_logsize=0)


@pytest.mark.parametrize(
    "run_args",
    [
        pytest.param(["http-proxy"]),  # HTTP proxy test runs on all platforms
        pytest.param(
            ["socks5-proxy"],
            marks=pytest.mark.skipif(
                sys.platform not in ["darwin", "linux"],
                reason="SOCKS5 proxy tests are only supported on macOS and Linux",
            ),
        ),
    ],
)
@pytest.mark.parametrize("proxy_running", [True, False])
def test_crashpad_crash_proxy(cmake, httpserver, run_args, proxy_running):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    expected_logsize = 0

    try:
        proxy_to_start = run_args[0] if proxy_running else None
        env, proxy_process, tmp_path = _setup_crashpad_proxy_test(
            cmake, httpserver, proxy_to_start
        )

        try:
            with httpserver.wait(timeout=10) as waiting:
                child = run(
                    tmp_path, "sentry_example", ["log", "crash"] + run_args, env=env
                )
                assert child.returncode  # well, it's a crash after all
        except AssertionError:
            expected_logsize = 0
            return

        assert waiting.result

        expected_logsize = 1
    finally:
        proxy_test_finally(expected_logsize, httpserver, proxy_process)


def test_crashpad_reinstall(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "crashpad"})

    # make sure we are isolated from previous runs
    shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))
    httpserver.expect_oneshot_request("/api/123456/minidump/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        child = run(tmp_path, "sentry_example", ["log", "reinstall", "crash"], env=env)
        assert child.returncode  # well, it's a crash after all

    assert waiting.result

    run(tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env)

    assert len(httpserver.log) == 1


@pytest.mark.skipif(
    sys.platform != "win32",
    reason="Test covers Windows-specific crashes which can only be covered via the Crashpad WER module",
)
# this test currently can't run on CI because the Windows-image doesn't properly support WER, if you want to run the
# test locally, invoke pytest with the --with_crashpad_wer option which is matched with this marker in the runtest setup
@pytest.mark.with_crashpad_wer
@pytest.mark.parametrize(
    "run_args",
    [
        # discarding via before-send or on-crash has no consequence for fast-fail crashes because they by-pass SEH and
        # thus the crash-handler gets no chance to invoke the FirstChanceHandler which in turn would trigger our hooks.
        (["stack-buffer-overrun"]),
        (["stack-buffer-overrun", "discarding-before-send"]),
        (["stack-buffer-overrun", "discarding-on-crash"]),
        (["fastfail"]),
        (["fastfail", "discarding-before-send"]),
        (["fastfail", "discarding-on-crash"]),
    ],
)
def test_crashpad_wer_crash(cmake, httpserver, run_args):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "crashpad"})

    # make sure we are isolated from previous runs
    shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))
    httpserver.expect_oneshot_request("/api/123456/minidump/").respond_with_data("OK")
    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        child = run(
            tmp_path,
            "sentry_example",
            [
                "log",
                "start-session",
                "attachment",
                "attach-view-hierarchy",
                "overflow-breadcrumbs",
            ]
            + run_args,
            env=env,
        )
        assert child.returncode  # well, it's a crash after all

    assert waiting.result

    # the session crash heuristic on Mac uses timestamps, so make sure we have
    # a small delay here
    time.sleep(1)

    run(tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env)

    assert len(httpserver.log) == 2
    outputs = (httpserver.log[0][0], httpserver.log[1][0])
    session, multipart = (
        (outputs[0].get_data(), outputs[1])
        if b'"type":"session"' in outputs[0].get_data()
        else (outputs[1].get_data(), outputs[0])
    )

    envelope = Envelope.deserialize(session)

    assert_session(envelope, {"status": "crashed", "errors": 1})
    assert_crashpad_upload(
        multipart, expect_attachment=True, expect_view_hierarchy=True
    )

    # Windows throttles WER crash reporting frequency, so let's wait a bit
    time.sleep(2)


@pytest.mark.parametrize(
    "run_args,build_args",
    [
        # if we crash, we want a dump
        (["attachment"], {"SENTRY_TRANSPORT_COMPRESSION": "Off"}),
        (["attachment"], {"SENTRY_TRANSPORT_COMPRESSION": "On"}),
        # if we crash and before-send doesn't discard, we want a dump
        pytest.param(
            ["attachment", "before-send"],
            {},
            marks=pytest.mark.skipif(
                sys.platform == "darwin",
                reason="crashpad doesn't provide SetFirstChanceExceptionHandler on macOS",
            ),
        ),
        # if on_crash() is non-discarding, a discarding before_send() is overruled, so we get a dump
        pytest.param(
            ["attachment", "discarding-before-send", "on-crash"],
            {},
            marks=pytest.mark.skipif(
                sys.platform == "darwin",
                reason="crashpad doesn't provide SetFirstChanceExceptionHandler on macOS",
            ),
        ),
        pytest.param(
            ["attach-after-init"],
            {},
            marks=pytest.mark.skipif(
                sys.platform == "darwin",
                reason="crashpad doesn't support dynamic attachments on macOS",
            ),
        ),
    ],
)
def test_crashpad_dumping_crash(cmake, httpserver, run_args, build_args):
    build_args.update({"SENTRY_BACKEND": "crashpad"})
    tmp_path = cmake(["sentry_example"], build_args)

    # make sure we are isolated from previous runs
    shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))
    httpserver.expect_oneshot_request("/api/123456/minidump/").respond_with_data("OK")
    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        child = run(
            tmp_path,
            "sentry_example",
            [
                "log",
                "start-session",
                "attach-view-hierarchy",
                "overflow-breadcrumbs",
                "crash",
            ]
            + run_args,
            env=env,
        )
        assert child.returncode  # well, it's a crash after all

    assert waiting.result

    # the session crash heuristic on Mac uses timestamps, so make sure we have
    # a small delay here
    time.sleep(1)

    run(tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env)

    assert len(httpserver.log) == 2
    session, multipart = (
        (httpserver.log[0][0], httpserver.log[1][0])
        if is_session_envelope(httpserver.log[0][0].get_data())
        else (httpserver.log[1][0], httpserver.log[0][0])
    )

    if build_args.get("SENTRY_TRANSPORT_COMPRESSION") == "On":
        assert_gzip_file_header(session.get_data())

    envelope = Envelope.deserialize(session.get_data())
    assert_session(envelope, {"status": "crashed", "errors": 1})
    assert_crashpad_upload(
        multipart, expect_attachment=True, expect_view_hierarchy=True
    )


@pytest.mark.parametrize(
    "build_args",
    [
        ({}),  # uses default of 64KiB
        pytest.param(
            {"SENTRY_HANDLER_STACK_SIZE": "16"},
            marks=pytest.mark.skipif(
                sys.platform != "win32",
                reason="handler stack size parameterization tests stack guarantee on windows only",
            ),
        ),
        pytest.param(
            {"SENTRY_HANDLER_STACK_SIZE": "32"},
            marks=pytest.mark.skipif(
                sys.platform != "win32",
                reason="handler stack size parameterization tests stack guarantee on windows only",
            ),
        ),
    ],
)
def test_crashpad_dumping_stack_overflow(cmake, httpserver, build_args):
    build_args.update({"SENTRY_BACKEND": "crashpad"})
    tmp_path = cmake(["sentry_example"], build_args)

    # make sure we are isolated from previous runs
    shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))
    httpserver.expect_oneshot_request("/api/123456/minidump/").respond_with_data("OK")
    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        child = run(
            tmp_path,
            "sentry_example",
            [
                "log",
                "start-session",
                "attachment",
                "attach-view-hierarchy",
                "stack-overflow",
            ],
            env=env,
        )
        assert child.returncode  # well, it's a crash after all

    assert waiting.result

    # the session crash heuristic on Mac uses timestamps, so make sure we have
    # a small delay here
    time.sleep(1)

    run(tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env)

    assert len(httpserver.log) == 2
    session, multipart = (
        (httpserver.log[0][0], httpserver.log[1][0])
        if is_session_envelope(httpserver.log[0][0].get_data())
        else (httpserver.log[1][0], httpserver.log[0][0])
    )

    envelope = Envelope.deserialize(session.get_data())
    assert_session(envelope, {"status": "crashed", "errors": 1})
    assert_crashpad_upload(
        multipart, expect_attachment=True, expect_view_hierarchy=True
    )


def is_session_envelope(data):
    return b'"type":"session"' in data


@pytest.mark.skipif(
    sys.platform == "darwin",
    reason="crashpad doesn't provide SetFirstChanceExceptionHandler on macOS",
)
@pytest.mark.parametrize(
    "run_args",
    [(["discarding-before-send"]), (["discarding-on-crash"])],
)
def test_crashpad_non_dumping_crash(cmake, httpserver, run_args):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "crashpad"})

    # make sure we are isolated from previous runs
    shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))
    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=5, raise_assertions=False) as waiting:
        child = run(
            tmp_path,
            "sentry_example",
            [
                "log",
                "start-session",
                "attachment",
                "overflow-breadcrumbs",
                "crash",
            ]
            + run_args,
            env=env,
        )
        assert child.returncode  # well, it's a crash after all

    assert waiting.result is False

    # the session crash heuristic on Mac uses timestamps, so make sure we have
    # a small delay here
    time.sleep(1)

    run(tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env)

    assert len(httpserver.log) == 1
    output = httpserver.log[0][0]
    session = output.get_data()
    envelope = Envelope.deserialize(session)

    assert_session(envelope, {"status": "abnormal", "errors": 0})


@pytest.mark.skipif(
    sys.platform == "linux", reason="linux clears the signal handlers on shutdown"
)
def test_crashpad_crash_after_shutdown(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "crashpad"})

    # make sure we are isolated from previous runs
    shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))
    httpserver.expect_oneshot_request("/api/123456/minidump/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        child = run(
            tmp_path,
            "sentry_example",
            ["log", "crash-after-shutdown"],
            env=env,
        )
        assert child.returncode  # well, it's a crash after all

    assert waiting.result

    # the session crash heuristic on Mac uses timestamps, so make sure we have
    # a small delay here
    time.sleep(1)

    run(tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env)

    assert len(httpserver.log) == 1

    assert_crashpad_upload(httpserver.log[0][0])


@pytest.mark.skipif(not flushes_state, reason="test needs state flushing")
def test_crashpad_dump_inflight(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "crashpad"})

    # make sure we are isolated from previous runs
    shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))
    httpserver.expect_oneshot_request("/api/123456/minidump/").respond_with_data("OK")
    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        child = run(
            tmp_path, "sentry_example", ["log", "capture-multiple", "crash"], env=env
        )
        assert child.returncode  # well, it's a crash after all

    assert waiting.result

    run(tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env)

    # we trigger 10 normal events, and 1 crash
    assert len(httpserver.log) >= 11


def test_disable_backend(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "crashpad"})

    # make sure we are isolated from previous runs
    shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    with httpserver.wait(timeout=5, raise_assertions=False) as waiting:
        child = run(
            tmp_path, "sentry_example", ["disable-backend", "log", "crash"], env=env
        )
        # we crash so process should return non-zero
        assert child.returncode

    # crashpad is disabled, and we are only crashing, so we expect the wait to timeout
    assert waiting.result is False

    run(tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env)

    # crashpad is disabled, and we are only crashing, so we expect no requests
    assert len(httpserver.log) == 0


@pytest.mark.skipif(
    sys.platform != "darwin" or not os.getenv("CI"),
    reason="retry mechanism test only runs on macOS in CI",
)
def test_crashpad_retry(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "crashpad"})

    subprocess.run(
        ["sudo", "ifconfig", "lo0", "down"]
    )  # Disables the loopback network interface

    # make sure we are isolated from previous runs
    shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))
    httpserver.expect_oneshot_request("/api/123456/minidump/").respond_with_data("OK")

    child = run(
        tmp_path, "sentry_example", ["log", "crash"], env=env
    )  # crash but fail to send data
    assert child.returncode  # well, it's a crash after all

    assert len(httpserver.log) == 0

    subprocess.run(
        ["sudo", "ifconfig", "lo0", "up"]
    )  # Enables the loopback network interface again
    # don't rmtree here, we don't want to be isolated (example should pick up previous crash from .sentry-native DB)
    # we also sleep to give Crashpad enough time to handle the previous crash
    child = run(
        tmp_path, "sentry_example", ["log", "sleep"], env=env
    )  # run without crashing to retry send

    assert len(httpserver.log) == 1
