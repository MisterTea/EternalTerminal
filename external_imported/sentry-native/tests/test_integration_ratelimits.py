import pytest
import os
from . import make_dsn, run
from .conditions import has_http

pytestmark = pytest.mark.skipif(not has_http, reason="tests need http")


def test_retry_after(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    # should respect retry-after also with `200` requests
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data(
        "OK", 200, {"retry-after": "60"}
    )
    run(tmp_path, "sentry_example", ["log", "capture-multiple"], check=True, env=env)
    assert len(httpserver.log) == 1

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data(
        "OK", 429, {"retry-after": "60"}
    )
    run(tmp_path, "sentry_example", ["log", "capture-multiple"], check=True, env=env)
    assert len(httpserver.log) == 2


def test_rate_limits(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))
    headers = {"X-Sentry-Rate-Limits": "60::organization"}

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data(
        "OK", 200, headers
    )
    run(tmp_path, "sentry_example", ["log", "capture-multiple"], check=True, env=env)
    assert len(httpserver.log) == 1

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data(
        "OK", 429, headers
    )
    run(tmp_path, "sentry_example", ["log", "capture-multiple"], check=True, env=env)
    assert len(httpserver.log) == 2
