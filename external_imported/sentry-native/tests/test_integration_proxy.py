import os
import shutil
import sys

import pytest

from . import (
    make_dsn,
    run,
)
from .assertions import (
    assert_failed_proxy_auth_request,
)
from .conditions import has_http
from .proxy import (
    closed_port,
    start_mitmdump,
    proxy_test_finally,
)

pytestmark = pytest.mark.skipif(not has_http, reason="tests need http")


def _setup_http_proxy_test(
    cmake, httpserver, proxy, proxy_auth=None, listen_host="127.0.0.1"
):
    if proxy:
        proxy_process, port = start_mitmdump(proxy, proxy_auth, listen_host=listen_host)
    else:
        proxy_process, port = None, None

    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver, proxy_host=True))
    if port is not None:
        env["SENTRY_TEST_PROXY_PORT"] = str(port)
    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")

    return env, proxy_process, tmp_path, port


def test_proxy_from_env(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    try:
        env, proxy_process, tmp_path, port = _setup_http_proxy_test(
            cmake, httpserver, "http-proxy"
        )
        env["http_proxy"] = f"http://127.0.0.1:{port}"
        env["https_proxy"] = f"http://127.0.0.1:{port}"

        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-event"],
            env=env,
        )

    finally:
        proxy_test_finally(1, httpserver, proxy_process)


def test_proxy_from_env_port_incorrect(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    try:
        env, proxy_process, tmp_path, port = _setup_http_proxy_test(
            cmake, httpserver, "http-proxy"
        )
        # Set env vars with a wrong port (offset by 1 from the actual proxy port)
        env["http_proxy"] = f"http://127.0.0.1:{port + 1}"
        env["https_proxy"] = f"http://127.0.0.1:{port + 1}"

        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-event"],
            env=env,
        )

    finally:
        proxy_test_finally(0, httpserver, proxy_process)


def test_proxy_auth(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    try:
        env, proxy_process, tmp_path, port = _setup_http_proxy_test(
            cmake, httpserver, "http-proxy", proxy_auth="user:password"
        )

        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-event", "http-proxy-auth"],
            env=dict(
                os.environ,
                SENTRY_DSN=make_dsn(httpserver, proxy_host=True),
                SENTRY_TEST_PROXY_PORT=str(port),
            ),
        )
    finally:
        proxy_test_finally(
            1,
            httpserver,
            proxy_process,
            assert_failed_proxy_auth_request,
        )


def test_proxy_auth_incorrect(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    try:
        env, proxy_process, tmp_path, port = _setup_http_proxy_test(
            cmake, httpserver, "http-proxy", proxy_auth="wrong:wrong"
        )

        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-event", "http-proxy-auth"],
            env=dict(
                os.environ,
                SENTRY_DSN=make_dsn(httpserver, proxy_host=True),
                SENTRY_TEST_PROXY_PORT=str(port),
            ),
        )
    finally:
        proxy_test_finally(
            0,
            httpserver,
            proxy_process,
            assert_failed_proxy_auth_request,
        )


def test_proxy_ipv6(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    try:
        env, proxy_process, tmp_path, port = _setup_http_proxy_test(
            cmake, httpserver, "http-proxy", listen_host="::1"
        )

        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-event", "http-proxy-ipv6"],
            env=env,
        )

    finally:
        proxy_test_finally(1, httpserver, proxy_process)


def test_proxy_set_empty(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    try:
        env, proxy_process, tmp_path, port = _setup_http_proxy_test(
            cmake, httpserver, "http-proxy"
        )
        # we start the proxy but expect it to remain unused
        env["http_proxy"] = f"http://127.0.0.1:{port}"
        env["https_proxy"] = f"http://127.0.0.1:{port}"

        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-event", "proxy-empty"],
            env=env,
        )

    finally:
        proxy_test_finally(1, httpserver, proxy_process, expected_proxy_logsize=0)


def test_proxy_https_not_http(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    try:
        # we start the proxy but expect it to remain unused (dsn is http, so shouldn't use https proxy)
        env, proxy_process, tmp_path, port = _setup_http_proxy_test(
            cmake, httpserver, "http-proxy"
        )
        env["https_proxy"] = f"http://127.0.0.1:{port}"

        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-event"],
            env=env,
        )

    finally:
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
def test_capture_proxy(cmake, httpserver, run_args, proxy_running):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    expected_logsize = 0

    try:
        proxy_to_start = run_args[0] if proxy_running else None
        env, proxy_process, tmp_path, port = _setup_http_proxy_test(
            cmake, httpserver, proxy_to_start
        )

        def do_run(effective_port):
            port_env = {"SENTRY_TEST_PROXY_PORT": str(effective_port)}
            run(
                tmp_path,
                "sentry_example",
                ["log", "capture-event"]
                + run_args,  # only passes if given proxy is running
                env=dict(
                    os.environ,
                    SENTRY_DSN=make_dsn(httpserver, proxy_host=True),
                    **port_env,
                ),
            )

        if proxy_running:
            do_run(port)
            expected_logsize = 1
        else:
            # Hold a port open without listening — connections are refused and
            # no other process can claim it while the test is running.
            with closed_port() as refused_port:
                do_run(refused_port)
            expected_logsize = 0
    finally:
        proxy_test_finally(expected_logsize, httpserver, proxy_process)
