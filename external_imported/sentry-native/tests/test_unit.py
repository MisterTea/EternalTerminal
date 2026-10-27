import os
import pytest
from . import run
from .conditions import has_http


def _skip_if_unsupported(unittest):
    # app_hang_end_to_end drives the real cross-thread RT-signal sampler and
    # unwinds a signal frame inside the handler. qemu-user does not emulate
    # thread-targeted signal delivery/unwinding faithfully, so the sample never
    # produces frames. It runs natively (incl. native arm64); skip only on qemu.
    if unittest == "app_hang_end_to_end" and os.environ.get("TEST_QEMU"):
        pytest.skip(
            "app_hang_end_to_end requires real signal delivery (unsupported under qemu-user)"
        )


def test_unit(cmake, unittest):
    if unittest in ["basic_transport_thread_name", "cache_keep"]:
        pytest.skip("excluded from unit test-suite")
    _skip_if_unsupported(unittest)
    cwd = cmake(
        ["sentry_test_unit"],
        {"SENTRY_BACKEND": "none", "SENTRY_TRANSPORT": "none"},
    )
    env = dict(os.environ)
    run(cwd, "sentry_test_unit", ["--no-summary", unittest], env=env)


@pytest.mark.skipif(not has_http, reason="tests need http transport")
def test_unit_transport(cmake, unittest):
    if unittest in [
        "custom_logger",
        "logger_enable_disable_functionality",
        "logger_level",
    ]:
        pytest.skip("excluded from transport test-suite")
    _skip_if_unsupported(unittest)

    cwd = cmake(
        ["sentry_test_unit"],
        {"SENTRY_BACKEND": "none"},
    )
    env = dict(os.environ)
    run(cwd, "sentry_test_unit", ["--no-summary", unittest], env=env)


def test_unit_with_test_path(cmake, unittest):
    if unittest in ["basic_transport_thread_name", "cache_keep"]:
        pytest.skip("excluded from unit test-suite")
    _skip_if_unsupported(unittest)
    cwd = cmake(
        ["sentry_test_unit"],
        {"SENTRY_BACKEND": "none", "SENTRY_TRANSPORT": "none"},
        cflags=['-DSENTRY_TEST_PATH_PREFIX=\\"./\\"'],
    )
    env = dict(os.environ)
    run(cwd, "sentry_test_unit", ["--no-summary", unittest], env=env)
