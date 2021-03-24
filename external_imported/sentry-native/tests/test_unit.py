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
    cwd = cmake(["sentry_test_unit"], {"SENTRY_BACKEND": "none"})
    env = dict(os.environ)
    run(cwd, "sentry_test_unit", ["--no-summary", unittest], check=True, env=env)
