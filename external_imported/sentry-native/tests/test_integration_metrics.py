import os

import pytest

from . import (
    make_dsn,
    run,
    Envelope,
    split_log_request_cond,
    is_metrics_envelope,
    SENTRY_VERSION,
)
from .assertions import (
    assert_event,
    assert_metrics,
)
from .conditions import has_http, has_breakpad, has_native, is_qemu

pytestmark = pytest.mark.skipif(not has_http, reason="tests need http")


def test_metrics_capture(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-metric"],
        env=env,
    )

    assert len(httpserver.log) == 1

    req = httpserver.log[0][0]
    body = req.get_data()

    envelope = Envelope.deserialize(body)
    envelope.print_verbose()
    assert_metrics(envelope, 1)


def test_metrics_all_types(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-metric-all-types"],
        env=env,
    )

    assert len(httpserver.log) == 1

    req = httpserver.log[0][0]
    body = req.get_data()

    envelope = Envelope.deserialize(body)
    envelope.print_verbose()
    assert_metrics(envelope, 3)

    # Verify the different metric types
    metrics = envelope.items[0].payload.json
    types_found = {item["type"] for item in metrics["items"]}
    assert types_found == {"counter", "gauge", "distribution"}


def test_metrics_with_custom_attributes(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "metric-with-attributes"],
        env=env,
    )

    assert len(httpserver.log) == 1

    req = httpserver.log[0][0]
    body = req.get_data()

    envelope = Envelope.deserialize(body)
    envelope.print_verbose()
    assert_metrics(envelope, 1)

    # Check custom attribute exists
    metric_item = envelope.items[0].payload.json["items"][0]
    attrs = metric_item["attributes"]
    assert "my.custom.attribute" in attrs
    assert attrs["my.custom.attribute"]["value"] == "my_value"
    assert attrs["my.custom.attribute"]["type"] == "string"


def test_metrics_timer(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "metrics-timer"],
        env=env,
    )

    # We expect 2 envelopes: one from the timer flush (after sleep), one at shutdown
    assert len(httpserver.log) == 2

    req_0 = httpserver.log[0][0]
    body_0 = req_0.get_data()
    envelope_0 = Envelope.deserialize(body_0)
    assert_metrics(envelope_0, 10)

    req_1 = httpserver.log[1][0]
    body_1 = req_1.get_data()
    envelope_1 = Envelope.deserialize(body_1)
    assert_metrics(envelope_1, 1)


def test_metrics_scoped_transaction(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        [
            "log",
            "metrics-scoped-transaction",
            "capture-transaction",
            "scope-transaction-event",
        ],
        env=env,
    )

    # Event, transaction, and metrics
    assert len(httpserver.log) == 3

    event_req = httpserver.log[0][0]
    event_body = event_req.get_data()
    event_envelope = Envelope.deserialize(event_body)
    assert_event(event_envelope)

    event_trace_id = event_envelope.items[0].payload.json["contexts"]["trace"][
        "trace_id"
    ]

    tx_req = httpserver.log[1][0]
    tx_body = tx_req.get_data()
    tx_envelope = Envelope.deserialize(tx_body)
    tx_trace_id = tx_envelope.items[0].payload.json["contexts"]["trace"]["trace_id"]
    assert tx_trace_id == event_trace_id

    metrics_req = httpserver.log[2][0]
    metrics_body = metrics_req.get_data()
    metrics_envelope = Envelope.deserialize(metrics_body)
    assert_metrics(metrics_envelope, 1, event_trace_id)


def test_before_send_metric(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-metric", "before-send-metric"],
        env=env,
    )

    assert len(httpserver.log) == 1
    req = httpserver.log[0][0]
    body = req.get_data()

    envelope = Envelope.deserialize(body)
    envelope.print_verbose()

    # Extract the metric item
    (metric_item,) = envelope.items

    assert metric_item.headers["type"] == "trace_metric"
    payload = metric_item.payload.json

    # Get the first metric item from the metrics payload
    metric_entry = payload["items"][0]
    attributes = metric_entry["attributes"]

    # Check that the before_send_metric callback added the expected attribute
    assert "coffeepot.size" in attributes
    assert attributes["coffeepot.size"]["value"] == "little"
    assert attributes["coffeepot.size"]["type"] == "string"


def test_before_send_metric_discard(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-metric", "discarding-before-send-metric"],
        env=env,
    )

    # metric should have been discarded
    assert len(httpserver.log) == 0


def test_metrics_disabled(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "disable-metrics", "capture-metric"],
        env=env,
    )

    # No metrics should be sent when feature is disabled
    assert len(httpserver.log) == 0


def test_metrics_event(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-metric", "capture-event"],
        env=env,
    )

    assert len(httpserver.log) == 2

    event_req = httpserver.log[0][0]
    event_body = event_req.get_data()
    event_envelope = Envelope.deserialize(event_body)
    assert_event(event_envelope)

    # ensure that the event and the metric are part of the same trace
    event_trace_id = event_envelope.items[0].payload.json["contexts"]["trace"][
        "trace_id"
    ]

    metrics_req = httpserver.log[1][0]
    metrics_body = metrics_req.get_data()
    metrics_envelope = Envelope.deserialize(metrics_body)
    assert_metrics(metrics_envelope, 1, event_trace_id)


def test_metrics_threaded(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")

    run(
        tmp_path,
        "sentry_example",
        ["log", "metrics-threads"],
        env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
    )

    total_count = 0

    for i in range(len(httpserver.log)):
        req = httpserver.log[i][0]
        body = req.get_data()

        envelope = Envelope.deserialize(body)
        assert_metrics(envelope)
        total_count += envelope.items[0].headers["item_count"]
    print(f"Total amount of captured metrics: {total_count}")
    assert total_count >= 100


def test_metrics_global_and_local_attributes_merge(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "set-global-attribute", "metric-with-attributes"],
        env=env,
    )

    assert len(httpserver.log) == 1
    req = httpserver.log[0][0]
    body = req.get_data()

    envelope = Envelope.deserialize(body)

    # Show what the envelope looks like if the test fails
    envelope.print_verbose()

    # Extract the metric item
    (metric_item,) = envelope.items

    assert metric_item.headers["type"] == "trace_metric"
    payload = metric_item.payload.json

    assert len(payload["items"]) == 1

    metric_entry = payload["items"][0]
    attributes = metric_entry["attributes"]

    # Verify local attribute (should overwrite global with same key)
    assert "my.custom.attribute" in attributes
    assert attributes["my.custom.attribute"]["value"] == "my_value"
    assert attributes["my.custom.attribute"]["type"] == "string"

    # Verify global attributes are present
    assert "global.attribute.bool" in attributes
    assert attributes["global.attribute.bool"]["value"] == True
    assert attributes["global.attribute.bool"]["type"] == "boolean"

    assert "global.attribute.int" in attributes
    assert attributes["global.attribute.int"]["value"] == 123
    assert attributes["global.attribute.int"]["type"] == "integer"

    assert "global.attribute.double" in attributes
    assert attributes["global.attribute.double"]["value"] == 1.23
    assert attributes["global.attribute.double"]["type"] == "double"

    assert "global.attribute.string" in attributes
    assert attributes["global.attribute.string"]["value"] == "my_global_value"
    assert attributes["global.attribute.string"]["type"] == "string"

    assert "global.attribute.array" in attributes
    assert attributes["global.attribute.array"]["value"] == ["item1", "item2"]
    assert attributes["global.attribute.array"]["type"] == "array"


def test_metrics_on_crash_none(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-metric", "crash"],
        expect_failure=True,
        env=env,
    )

    # metric should have been discarded since we have no backend to hook into the crash
    assert len(httpserver.log) == 0


@pytest.mark.parametrize(
    "backend",
    [
        "inproc",
        pytest.param(
            "breakpad",
            marks=pytest.mark.skipif(
                not has_breakpad or is_qemu, reason="breakpad backend not available"
            ),
        ),
    ],
)
def test_metrics_on_crash(cmake, httpserver, backend):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": backend})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-metric", "crash"],
        expect_failure=True,
        env=env,
    )

    with httpserver.wait(timeout=10):
        run(
            tmp_path,
            "sentry_example",
            ["log", "no-setup"],
            env=env,
        )

    # we expect 1 envelope with the metric, and 1 for the crash
    assert len(httpserver.log) == 2
    metrics_request, crash_request = split_log_request_cond(
        httpserver.log, is_metrics_envelope
    )
    metrics = metrics_request.get_data()

    metrics_envelope = Envelope.deserialize(metrics)

    assert metrics_envelope is not None
    assert_metrics(metrics_envelope, 1)


@pytest.mark.skipif(not has_native or is_qemu, reason="test needs native backend")
@pytest.mark.parametrize("rerun", [True, False], ids=["rerun", "no-rerun"])
def test_metrics_on_crash_native(cmake, httpserver, rerun):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    with httpserver.wait(timeout=10):
        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-metric", "crash"],
            expect_failure=True,
            env=env,
        )

        if rerun:
            # Rerun the application without crashing to ensure that the second run
            # and a second daemon do not conflict with each other
            run(
                tmp_path,
                "sentry_example",
                ["log", "no-setup"],
                env=env,
            )

    # we expect 1 envelope with the metric, and 1 for the crash
    assert len(httpserver.log) == 2
    metrics_request, crash_request = split_log_request_cond(
        httpserver.log, is_metrics_envelope
    )
    metrics = metrics_request.get_data()

    metrics_envelope = Envelope.deserialize(metrics)

    assert metrics_envelope is not None
    assert_metrics(metrics_envelope, 1)
