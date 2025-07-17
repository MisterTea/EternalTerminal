import os
import pytest
from . import run
from .conditions import has_http


def test_unit(cmake, unittest):
    cwd = cmake(
        ["sentry_test_unit"], {"SENTRY_BACKEND": "none", "SENTRY_TRANSPORT": "none"}
    )
    env = dict(os.environ)
    run(cwd, "sentry_test_unit", ["--no-summary", unittest], check=True, env=env)


@pytest.mark.skipif(not has_http, reason="tests need http transport")
def test_unit_transport(cmake, unittest):
    if unittest in ["custom_logger"]:
        pytest.skip("excluded from transport test-suite")

    cwd = cmake(["sentry_test_unit"], {"SENTRY_BACKEND": "none"})
    env = dict(os.environ)
    run(cwd, "sentry_test_unit", ["--no-summary", unittest], check=True, env=env)


def test_unit_with_test_path(cmake, unittest):
    cwd = cmake(
        ["sentry_test_unit"],
        {"SENTRY_BACKEND": "none", "SENTRY_TRANSPORT": "none"},
        cflags=["-DSENTRY_TEST_PATH_PREFIX=./"],
    )
    env = dict(os.environ)
    run(cwd, "sentry_test_unit", ["--no-summary", unittest], check=True, env=env)
