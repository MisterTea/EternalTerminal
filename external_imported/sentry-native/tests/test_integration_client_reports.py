import os
import subprocess

import pytest

from . import make_dsn, run, Envelope
from .assertions import (
    assert_client_report,
    assert_event,
    assert_session,
)
from .conditions import has_files, has_http

pytestmark = pytest.mark.skipif(not has_http, reason="tests need http")


def test_client_report_none(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-event"],
        env=env,
    )

    assert len(httpserver.log) == 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())

    assert_event(envelope)
    for item in envelope:
        assert item.headers.get("type") != "client_report"


def test_client_report_before_send(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    # The event is discarded by before_send. The session envelope sent at
    # shutdown acts as a carrier for the client report.
    run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "discarding-before-send", "capture-event"],
        env=env,
    )

    assert len(httpserver.log) == 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())

    assert_session(envelope)
    assert_client_report(
        envelope,
        [{"reason": "before_send", "category": "error", "quantity": 1}],
    )


def test_client_report_ratelimit(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    # The first event gets through but triggers a rate limit for the "error"
    # category only. The error items of the remaining 9 events are filtered out
    # during serialization, but their session updates still pass through (not
    # rate-limited). Each subsequent envelope carries client report discards
    # incrementally, so we aggregate across all envelopes.
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data(
        "OK", headers={"X-Sentry-Rate-Limits": "60:error:organization"}
    )
    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "capture-multiple"],
        env=env,
    )

    # First envelope: event + session (before rate limit).
    # No event items in subsequent envelopes (rate-limited), but session
    # updates still go through.
    assert len(httpserver.log) >= 2

    total_discards = {}
    for req, _resp in httpserver.log:
        envelope = Envelope.deserialize(req.get_data())
        for item in envelope:
            if item.headers.get("type") != "client_report" or not item.payload.json:
                continue
            for entry in item.payload.json.get("discarded_events", []):
                key = (entry["reason"], entry["category"])
                total_discards[key] = total_discards.get(key, 0) + entry["quantity"]

    assert total_discards == {("ratelimit_backoff", "error"): 9}


def test_client_report_ratelimit_then_send_error(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    # First request rate-limits sessions. Second request returns 400.
    # The session item is filtered during serialization (ratelimit_backoff).
    # The event item is sent but rejected (send_error). Each item must be
    # counted exactly once under the correct reason.
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data(
        "OK", headers={"X-Sentry-Rate-Limits": "60:session:organization"}
    )
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data(
        "Bad Request", status=400
    )
    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "capture-multiple"],
        env=env,
    )

    total_discards = {}
    for req, _resp in httpserver.log:
        envelope = Envelope.deserialize(req.get_data())
        for item in envelope:
            if item.headers.get("type") != "client_report" or not item.payload.json:
                continue
            for entry in item.payload.json.get("discarded_events", []):
                key = (entry["reason"], entry["category"])
                total_discards[key] = total_discards.get(key, 0) + entry["quantity"]

    # Session updates rate-limited after the first envelope
    assert ("ratelimit_backoff", "session") in total_discards
    # Second event rejected by the server
    assert ("send_error", "error") in total_discards
    # Sessions must NOT also be counted as send_error
    assert ("send_error", "session") not in total_discards


def test_client_report_sample_rate(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(
        os.environ,
        SENTRY_DSN=make_dsn(httpserver),
        SENTRY_SAMPLE_RATE="0.0",
    )

    # Event is discarded by sample rate. The session at shutdown carries
    # the client report.
    run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "capture-event"],
        env=env,
    )

    assert len(httpserver.log) == 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())

    assert_session(envelope)
    assert_client_report(
        envelope,
        [{"reason": "sample_rate", "category": "error", "quantity": 1}],
    )


def test_client_report_before_send_log(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    # Log is discarded by before_send_log. The event envelope carries
    # the client report.
    run(
        tmp_path,
        "sentry_example",
        [
            "log",
            "discarding-before-send-log",
            "capture-log",
            "capture-event",
        ],
        env=env,
    )

    assert len(httpserver.log) == 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())

    assert_event(envelope)
    assert_client_report(
        envelope,
        [{"reason": "before_send", "category": "log_item", "quantity": 1}],
    )


def test_client_report_before_send_metric(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    # Metric is discarded by before_send_metric. The event envelope carries
    # the client report.
    run(
        tmp_path,
        "sentry_example",
        [
            "log",
            "enable-metrics",
            "discarding-before-send-metric",
            "capture-metric",
            "capture-event",
        ],
        env=env,
    )

    assert len(httpserver.log) == 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())

    assert_event(envelope)
    assert_client_report(
        envelope,
        [{"reason": "before_send", "category": "trace_metric", "quantity": 1}],
    )


def test_client_report_before_send_transaction(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    # Transaction is discarded by before_transaction. The session at
    # shutdown carries the client report.
    run(
        tmp_path,
        "sentry_example",
        [
            "log",
            "start-session",
            "capture-transaction",
            "discarding-before-transaction",
        ],
        env=env,
    )

    assert len(httpserver.log) == 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())

    assert_session(envelope)
    assert_client_report(
        envelope,
        [{"reason": "before_send", "category": "transaction", "quantity": 1}],
    )


def test_client_report_send_error_preserves_pending(cmake, httpserver):
    """Client report counts attached to a failed envelope are preserved."""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data(
        "Internal Server Error", status=500
    )
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data(
        "Internal Server Error", status=500
    )
    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "capture-multiple"],
        env=env,
    )

    # Events 1-2 fail (500) -> send_error discards accumulate.
    # Event 3+ succeed and deliver the accumulated client report.
    assert len(httpserver.log) > 2
    total_discards = {}
    for req, _resp in httpserver.log[2:]:
        envelope = Envelope.deserialize(req.get_data())
        for item in envelope:
            if item.headers.get("type") != "client_report" or not item.payload.json:
                continue
            for entry in item.payload.json.get("discarded_events", []):
                key = (entry["reason"], entry["category"])
                total_discards[key] = total_discards.get(key, 0) + entry["quantity"]

    assert total_discards[("send_error", "error")] == 2
    assert total_discards[("send_error", "session")] == 2


def test_client_report_send_error(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data(
        "Internal Server Error", status=500
    )
    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "capture-event"],
        env=env,
    )

    assert len(httpserver.log) == 2
    envelope = Envelope.deserialize(httpserver.log[1][0].get_data())

    assert_session(envelope)
    assert_client_report(
        envelope,
        [{"reason": "send_error", "category": "error", "quantity": 1}],
    )


def test_client_report_content_too_large(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data(
        "Content Too Large", status=413
    )
    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    result = run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "capture-event"],
        env=env,
        stderr=subprocess.PIPE,
    )

    assert len(httpserver.log) == 2
    envelope = Envelope.deserialize(httpserver.log[1][0].get_data())

    assert_session(envelope)
    assert_client_report(
        envelope,
        [{"reason": "send_error", "category": "error", "quantity": 1}],
    )

    assert b"HTTP 413 Content Too Large" in result.stderr


@pytest.mark.skipif(not has_files, reason="test needs a local filesystem")
def test_client_report_with_retry(cmake, httpserver, unreachable_dsn):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})
    cache_dir = tmp_path.joinpath(".sentry-native/cache")

    # Run 1: event discarded by before_send (client report recorded).
    # The session at shutdown picks up the client report, but send fails
    # (unreachable), so the session+client_report is cached for retry.
    run(
        tmp_path,
        "sentry_example",
        [
            "log",
            "http-retry",
            "start-session",
            "discarding-before-send",
            "capture-event",
        ],
        env=dict(os.environ, SENTRY_DSN=unreachable_dsn),
    )

    assert cache_dir.exists()
    cache_files = list(cache_dir.glob("*.envelope"))
    assert len(cache_files) == 1

    # Run 2: retry succeeds — the retried session should carry the
    # client report from run 1.
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        run(
            tmp_path,
            "sentry_example",
            ["log", "http-retry", "no-setup"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    assert len(httpserver.log) == 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())
    assert_session(envelope)
    assert_client_report(
        envelope,
        [{"reason": "before_send", "category": "error", "quantity": 1}],
    )

    cache_files = list(cache_dir.glob("*.envelope"))
    assert len(cache_files) == 0
