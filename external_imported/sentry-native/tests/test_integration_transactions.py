import os
import time
import uuid

import pytest

from . import (
    make_dsn,
    run,
    Envelope,
    SENTRY_VERSION,
)
from .assertions import (
    assert_meta,
    assert_event,
    assert_gzip_content_encoding,
    assert_gzip_file_header,
)
from .conditions import has_http

pytestmark = pytest.mark.skipif(not has_http, reason="tests need http")

RFC3339_FORMAT = "%Y-%m-%dT%H:%M:%S.%fZ"
UPSTREAM_TRACE_ID = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
UPSTREAM_PARENT_SPAN_ID = "bbbbbbbbbbbbbbbb"


@pytest.mark.parametrize(
    "build_args",
    [
        ({}),  # SENTRY_TRANSPORT_COMPRESSION=Off (default, cached)
        ({"SENTRY_TRANSPORT_COMPRESSION": "On"}),
    ],
)
def test_transaction_only(cmake, httpserver, build_args):
    build_args.update({"SENTRY_BACKEND": "none"})
    tmp_path = cmake(["sentry_example"], build_args)

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-transaction"],
        env=env,
    )

    assert len(httpserver.log) == 1
    req = httpserver.log[0][0]
    body = req.get_data()

    if build_args.get("SENTRY_TRANSPORT_COMPRESSION") == "On":
        assert_gzip_content_encoding(req)
        assert_gzip_file_header(body)

    envelope = Envelope.deserialize(body)

    # Show what the envelope looks like if the test fails.
    envelope.print_verbose()

    # The transaction is overwritten.
    assert_meta(
        envelope,
        transaction="little.teapot",
    )

    # Extract the one-and-only-item
    (event,) = envelope.items

    assert event.headers["type"] == "transaction"
    payload = event.payload.json

    # See https://develop.sentry.dev/sdk/data-model/event-payloads/contexts/#trace-context
    trace_context = payload["contexts"]["trace"]

    assert (
        trace_context["op"] == "Short and stout here is my handle and here is my spout"
    )

    assert trace_context["trace_id"]
    trace_id = uuid.UUID(hex=trace_context["trace_id"])
    assert trace_id

    assert trace_context["span_id"]
    assert trace_context["status"] == "ok"

    start_timestamp = time.strptime(payload["start_timestamp"], RFC3339_FORMAT)
    assert start_timestamp
    timestamp = time.strptime(payload["timestamp"], RFC3339_FORMAT)
    assert timestamp >= start_timestamp

    assert trace_context["data"] == {"url": "https://example.com"}


def test_before_transaction_callback(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-transaction", "before-transaction"],
        env=env,
    )

    assert len(httpserver.log) == 1
    req = httpserver.log[0][0]
    body = req.get_data()

    envelope = Envelope.deserialize(body)

    # Show what the envelope looks like if the test fails.
    envelope.print_verbose()

    # callback has changed from default teapot to coffeepot
    assert_meta(
        envelope,
        transaction="little.coffeepot",
    )

    # Extract the one-and-only-item
    (event,) = envelope.items

    assert event.headers["type"] == "transaction"
    payload = event.payload.json

    # See https://develop.sentry.dev/sdk/data-model/event-payloads/contexts/#trace-context
    trace_context = payload["contexts"]["trace"]

    assert (
        trace_context["op"] == "Short and stout here is my handle and here is my spout"
    )

    assert trace_context["trace_id"]
    trace_id = uuid.UUID(hex=trace_context["trace_id"])
    assert trace_id

    assert trace_context["span_id"]
    assert trace_context["status"] == "ok"

    start_timestamp = time.strptime(payload["start_timestamp"], RFC3339_FORMAT)
    assert start_timestamp
    timestamp = time.strptime(payload["timestamp"], RFC3339_FORMAT)
    assert timestamp >= start_timestamp

    assert trace_context["data"] == {"url": "https://example.com"}


def test_before_transaction_discard(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-transaction", "discarding-before-transaction"],
        env=env,
    )

    # transaction should have been discarded
    assert len(httpserver.log) == 0


def test_transaction_event(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-transaction", "capture-event"],
        env=env,
    )

    assert len(httpserver.log) == 2
    tx_req = httpserver.log[1][0]
    tx_body = tx_req.get_data()

    event_req = httpserver.log[0][0]
    event_body = event_req.get_data()

    tx_envelope = Envelope.deserialize(tx_body)
    event_envelope = Envelope.deserialize(event_body)

    # Show what the envelope looks like if the test fails.
    tx_envelope.print_verbose()

    # The transaction is overwritten.
    assert_meta(
        tx_envelope,
        transaction="little.teapot",
    )

    # Extract the transaction and event items
    (tx_event,) = tx_envelope.items
    (event,) = event_envelope.items

    assert tx_event.headers["type"] == "transaction"
    payload = tx_event.payload.json

    # See https://develop.sentry.dev/sdk/data-model/event-payloads/contexts/#trace-context
    trace_context = payload["contexts"]["trace"]

    assert trace_context["trace_id"]
    tx_trace_id = uuid.UUID(hex=trace_context["trace_id"])
    assert tx_trace_id
    event_trace_id = uuid.UUID(hex=event.payload.json["contexts"]["trace"]["trace_id"])
    # by default, transactions and events will have different trace id's because transactions create their own traces
    # unless a client explicitly called `sentry_set_trace()` (which transfers the burden of management to the caller).
    assert tx_trace_id != event_trace_id
    assert_event(event_envelope, "Hello World!", "")
    # non-scoped tx should differ in span_id from the event span_id
    assert trace_context["span_id"]
    assert (
        trace_context["span_id"] != event.payload.json["contexts"]["trace"]["span_id"]
    )


def test_transaction_trace_header(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "set-trace", "capture-transaction"],
        env=env,
    )

    assert len(httpserver.log) == 1
    event_req = httpserver.log[0][0]
    event_body = event_req.get_data()

    event_envelope = Envelope.deserialize(event_body)

    # Extract the one-and-only-item
    (event,) = event_envelope.items

    payload = event.payload.json

    # See https://develop.sentry.dev/sdk/data-model/event-payloads/contexts/#trace-context
    trace_context = payload["contexts"]["trace"]

    # See https://develop.sentry.dev/sdk/telemetry/traces/dynamic-sampling-context/#dsc-specification
    trace_header = event_envelope.headers["trace"]
    # assert for random value to exist & be in the expected range
    assert ("sample_rand" in trace_header) and (0 <= trace_header["sample_rand"] < 1)
    del trace_header["sample_rand"]
    assert trace_header == {
        "environment": "development",
        "public_key": "uiaeosnrtdy",
        "release": "test-example-release",
        "sample_rate": 1,
        "sampled": "true",
        "trace_id": "aaaabbbbccccddddeeeeffff00001111",
        "transaction": "little.teapot",
    }

    assert trace_context["trace_id"] == trace_header["trace_id"]


def test_event_trace_header(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "set-trace", "capture-event"],
        env=env,
    )

    assert len(httpserver.log) == 1
    event_req = httpserver.log[0][0]
    event_body = event_req.get_data()

    event_envelope = Envelope.deserialize(event_body)

    # Extract the one-and-only-item
    (event,) = event_envelope.items

    payload = event.payload.json

    # See https://develop.sentry.dev/sdk/data-model/event-payloads/contexts/#trace-context
    trace_context = payload["contexts"]["trace"]

    # See https://develop.sentry.dev/sdk/telemetry/traces/dynamic-sampling-context/#dsc-specification
    trace_header = event_envelope.headers["trace"]
    # assert for random value to exist & be in the expected range
    assert ("sample_rand" in trace_header) and (0 <= trace_header["sample_rand"] < 1)
    del trace_header["sample_rand"]
    assert trace_header == {
        "environment": "development",
        "public_key": "uiaeosnrtdy",
        "release": "test-example-release",
        "sample_rate": 0,  # since we don't capture-transaction
        "sampled": "false",  # now sample_rand >= traces_sample_rate (=0)
        "trace_id": "aaaabbbbccccddddeeeeffff00001111",
    }

    assert trace_context["trace_id"] == trace_header["trace_id"]


def test_set_trace_event(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "set-trace", "capture-event"],
        env=env,
    )

    assert len(httpserver.log) == 1
    event_req = httpserver.log[0][0]
    event_body = event_req.get_data()

    event_envelope = Envelope.deserialize(event_body)

    # Extract the one-and-only-item
    (event,) = event_envelope.items

    payload = event.payload.json

    # See https://develop.sentry.dev/sdk/data-model/event-payloads/contexts/#trace-context
    trace_context = payload["contexts"]["trace"]

    assert_event(event_envelope, "Hello World!", "aaaabbbbccccddddeeeeffff00001111")
    assert trace_context["parent_span_id"]
    assert trace_context["parent_span_id"] == "f0f0f0f0f0f0f0f0"


def test_set_trace_transaction_scoped_event(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-transaction", "scope-transaction-event"],
        env=env,
    )

    assert len(httpserver.log) == 2
    tx_req = httpserver.log[1][0]
    tx_body = tx_req.get_data()

    event_req = httpserver.log[0][0]
    event_body = event_req.get_data()

    tx_envelope = Envelope.deserialize(tx_body)
    event_envelope = Envelope.deserialize(event_body)

    # Show what the envelope looks like if the test fails.
    tx_envelope.print_verbose()

    # The transaction is overwritten.
    assert_meta(
        tx_envelope,
        transaction="little.teapot",
    )

    # Extract the transaction and event items
    (tx_event,) = tx_envelope.items
    (event,) = event_envelope.items

    assert tx_event.headers["type"] == "transaction"
    payload = tx_event.payload.json

    # See https://develop.sentry.dev/sdk/data-model/event-payloads/contexts/#trace-context
    trace_context = payload["contexts"]["trace"]

    assert trace_context["trace_id"]
    tx_trace_id = uuid.UUID(hex=trace_context["trace_id"])
    assert tx_trace_id
    event_trace_id = uuid.UUID(hex=event.payload.json["contexts"]["trace"]["trace_id"])
    # by default, transactions and events should have the same trace id (picked up from the propagation context)
    assert tx_trace_id == event_trace_id
    assert_event(event_envelope, "Hello World!", trace_context["trace_id"])
    # scoped tx should have the same span_id as the event span_id
    assert trace_context["span_id"]
    assert (
        trace_context["span_id"] == event.payload.json["contexts"]["trace"]["span_id"]
    )


def test_set_trace_transaction_update_from_header_event(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        [
            "log",
            "capture-transaction",
            "update-tx-from-header",
            "scope-transaction-event",
        ],
        env=env,
    )

    assert len(httpserver.log) == 2
    tx_req = httpserver.log[1][0]
    tx_body = tx_req.get_data()

    event_req = httpserver.log[0][0]
    event_body = event_req.get_data()

    tx_envelope = Envelope.deserialize(tx_body)
    event_envelope = Envelope.deserialize(event_body)

    # Show what the envelope looks like if the test fails.
    tx_envelope.print_verbose()

    # The transaction is overwritten.
    assert_meta(
        tx_envelope,
        transaction="little.teapot",
    )

    # Extract the transaction and event items
    (tx_event,) = tx_envelope.items
    (event,) = event_envelope.items

    assert tx_event.headers["type"] == "transaction"
    payload = tx_event.payload.json

    # See https://develop.sentry.dev/sdk/data-model/event-payloads/contexts/#trace-context
    trace_context = payload["contexts"]["trace"]
    expected_trace_id = "2674eb52d5874b13b560236d6c79ce8a"
    expected_parent_span_id = "a0f9fdf04f1a63df"

    assert trace_context["trace_id"]
    assert event.payload.json["contexts"]["trace"]["trace_id"]
    # Event should have the same trace_id as scoped span (set by update_from_header)
    assert (
        trace_context["trace_id"]
        == event.payload.json["contexts"]["trace"]["trace_id"]
        == expected_trace_id
    )
    assert_event(event_envelope, "Hello World!", trace_context["trace_id"])
    # scoped tx should have the same span_id as the event span_id
    assert trace_context["span_id"]
    assert (
        trace_context["span_id"] == event.payload.json["contexts"]["trace"]["span_id"]
    )
    # tx gets parent span_id from update_from_header
    assert trace_context["parent_span_id"]
    assert trace_context["parent_span_id"] == expected_parent_span_id


@pytest.mark.parametrize(
    "strict,incoming_baggage_flag,continues,expected_dsc",
    [
        pytest.param(
            True,
            "incoming-baggage",
            True,
            {
                "environment": "upstream",
                "org_id": "123456",
                "release": "upstream-app@1.0",
            },
            id="strict-matching-org-continues",
        ),
        pytest.param(
            False,
            "incoming-baggage-mismatch",
            False,
            {
                "environment": "development",
                "org_id": "123456",
                "release": "test-example-release",
            },
            id="lenient-mismatched-org-forks",
        ),
        pytest.param(
            True,
            None,
            False,
            {
                "environment": "development",
                "org_id": "123456",
                "release": "test-example-release",
            },
            id="strict-no-baggage-forks",
        ),
        pytest.param(
            False,
            None,
            True,
            {
                "environment": "development",
                "org_id": "123456",
                "release": "test-example-release",
            },
            id="lenient-no-baggage-continues",
        ),
    ],
)
def test_strict_trace_continuation(
    cmake, httpserver, strict, incoming_baggage_flag, continues, expected_dsc
):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    args = [
        "log",
        "org-id",
        "capture-transaction",
        "incoming-trace",
    ]
    if strict:
        args.append("strict-trace-continuation")
    if incoming_baggage_flag:
        args.append(incoming_baggage_flag)

    run(tmp_path, "sentry_example", args, env=env)

    assert len(httpserver.log) == 1
    event_envelope = Envelope.deserialize(httpserver.log[0][0].get_data())
    event_envelope.print_verbose()

    (event,) = event_envelope.items
    assert event.headers["type"] == "transaction"
    payload = event.payload.json
    trace_context = payload["contexts"]["trace"]
    trace_header = event_envelope.headers["trace"]

    assert "incoming_dsc" not in payload
    assert trace_header["trace_id"] == trace_context["trace_id"]
    for key, value in expected_dsc.items():
        assert trace_header[key] == value

    if continues:
        assert trace_context["trace_id"] == UPSTREAM_TRACE_ID
        assert trace_context["parent_span_id"] == UPSTREAM_PARENT_SPAN_ID
    else:
        assert trace_context["trace_id"] != UPSTREAM_TRACE_ID
        assert "parent_span_id" not in trace_context
