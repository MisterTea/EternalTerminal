import os

import pytest

from . import (
    make_dsn,
    run,
    Envelope,
    split_log_request_cond,
    is_logs_envelope,
    SENTRY_VERSION,
)
from .assertions import (
    assert_event,
    assert_logs,
)
from .conditions import has_http, has_breakpad, has_native, is_qemu

pytestmark = pytest.mark.skipif(not has_http, reason="tests need http")


def test_logs_timer(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")

    run(
        tmp_path,
        "sentry_example",
        ["log", "logs-timer"],
        env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
    )

    assert len(httpserver.log) == 2

    req_0 = httpserver.log[0][0]
    body_0 = req_0.get_data()

    envelope_0 = Envelope.deserialize(body_0)
    assert_logs(envelope_0, 10)

    req_1 = httpserver.log[1][0]
    body_1 = req_1.get_data()

    envelope_1 = Envelope.deserialize(body_1)
    assert_logs(envelope_1)


def test_logs_event(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-log", "capture-event"],
        env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
    )

    assert len(httpserver.log) == 2

    event_req = httpserver.log[0][0]
    event_body = event_req.get_data()

    event_envelope = Envelope.deserialize(event_body)
    assert_event(event_envelope)
    # ensure that the event and the log are part of the same trace
    event_trace_id = event_envelope.items[0].payload.json["contexts"]["trace"][
        "trace_id"
    ]

    log_req = httpserver.log[1][0]
    log_body = log_req.get_data()

    log_envelope = Envelope.deserialize(log_body)
    assert_logs(log_envelope, 1, event_trace_id)


def test_logs_scoped_transaction(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")

    run(
        tmp_path,
        "sentry_example",
        [
            "log",
            "logs-scoped-transaction",
            "capture-transaction",
            "scope-transaction-event",
        ],
        env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
    )

    assert len(httpserver.log) == 3

    event_req = httpserver.log[0][0]
    event_body = event_req.get_data()

    event_envelope = Envelope.deserialize(event_body)
    assert_event(event_envelope)
    # ensure that the event and the log are part of the same trace
    event_trace_id = event_envelope.items[0].payload.json["contexts"]["trace"][
        "trace_id"
    ]

    tx_req = httpserver.log[1][0]
    tx_body = tx_req.get_data()

    tx_envelope = Envelope.deserialize(tx_body)
    # ensure that the transaction, event, and logs are part of the same trace
    tx_trace_id = tx_envelope.items[0].payload.json["contexts"]["trace"]["trace_id"]
    assert tx_trace_id == event_trace_id

    log_req = httpserver.log[2][0]
    log_body = log_req.get_data()

    log_envelope = Envelope.deserialize(log_body)
    assert_logs(log_envelope, 2, event_trace_id)


def test_logs_threaded(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")

    run(
        tmp_path,
        "sentry_example",
        ["log", "logs-threads"],
        env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
    )

    total_count = 0

    for i in range(len(httpserver.log)):
        req = httpserver.log[i][0]
        body = req.get_data()

        envelope = Envelope.deserialize(body)
        assert_logs(envelope)
        total_count += envelope.items[0].headers["item_count"]
    print(f"Total amount of captured logs: {total_count}")
    assert total_count >= 100


def test_before_send_log(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-log", "before-send-log"],
        env=env,
    )

    assert len(httpserver.log) == 1
    req = httpserver.log[0][0]
    body = req.get_data()

    envelope = Envelope.deserialize(body)

    # Show what the envelope looks like if the test fails.
    envelope.print_verbose()

    # Extract the log item
    (log_item,) = envelope.items

    assert log_item.headers["type"] == "log"
    payload = log_item.payload.json

    # Get the first log item from the logs payload
    log_entry = payload["items"][0]
    attributes = log_entry["attributes"]

    # Check that the before_send_log callback added the expected attribute
    assert "coffeepot.size" in attributes
    assert attributes["coffeepot.size"]["value"] == "little"
    assert attributes["coffeepot.size"]["type"] == "string"


def test_before_send_log_discard(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-log", "discarding-before-send-log"],
        env=env,
    )

    # log should have been discarded
    assert len(httpserver.log) == 0


def test_logs_on_crash_none(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-log", "crash"],
        expect_failure=True,
        env=env,
    )

    # log should have been discarded since we have no backend to hook into the crash
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
def test_logs_on_crash(cmake, httpserver, backend):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": backend})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-log", "crash"],
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

    # we expect 1 envelope with the log, and 1 for the crash
    assert len(httpserver.log) == 2
    logs_request, crash_request = split_log_request_cond(
        httpserver.log, is_logs_envelope
    )
    logs = logs_request.get_data()

    logs_envelope = Envelope.deserialize(logs)

    assert logs_envelope is not None
    assert_logs(logs_envelope, 1)


@pytest.mark.skipif(not has_native or is_qemu, reason="test needs native backend")
@pytest.mark.parametrize("rerun", [True, False], ids=["rerun", "no-rerun"])
def test_logs_on_crash_native(cmake, httpserver, rerun):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    with httpserver.wait(timeout=10):
        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-log", "crash"],
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

    # we expect 1 envelope with the log, and 1 for the crash
    assert len(httpserver.log) == 2
    logs_request, crash_request = split_log_request_cond(
        httpserver.log, is_logs_envelope
    )
    logs = logs_request.get_data()

    logs_envelope = Envelope.deserialize(logs)

    assert logs_envelope is not None
    assert_logs(logs_envelope, 1)


def test_logs_with_custom_attributes(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "log-attributes"],
        env=env,
    )

    assert len(httpserver.log) == 1
    req = httpserver.log[0][0]
    body = req.get_data()

    envelope = Envelope.deserialize(body)

    # Show what the envelope looks like if the test fails
    envelope.print_verbose()

    # Extract the log item
    (log_item,) = envelope.items

    assert log_item.headers["type"] == "log"
    payload = log_item.payload.json

    # We expect 3 log entries based on the example
    assert len(payload["items"]) == 3

    # Test 1: Log with custom attributes and format string
    log_entry_0 = payload["items"][0]
    assert log_entry_0["body"] == "logging with 3 custom attributes"
    attributes_0 = log_entry_0["attributes"]

    # Check custom attributes exist
    assert "my.custom.attribute" in attributes_0
    assert attributes_0["my.custom.attribute"]["value"] == "my_attribute"
    assert attributes_0["my.custom.attribute"]["type"] == "string"

    assert "number.first" in attributes_0
    assert attributes_0["number.first"]["value"] == 2**63 - 1  # INT64_MAX
    assert attributes_0["number.first"]["type"] == "integer"
    assert attributes_0["number.first"]["unit"] == "fermions"

    assert "number.second" in attributes_0
    assert attributes_0["number.second"]["value"] == -(2**63)  # INT64_MIN
    assert attributes_0["number.second"]["type"] == "integer"
    assert attributes_0["number.second"]["unit"] == "bosons"

    # Check that format parameters were parsed
    assert "sentry.message.parameter.0" in attributes_0
    assert attributes_0["sentry.message.parameter.0"]["value"] == 3
    assert attributes_0["sentry.message.parameter.0"]["type"] == "integer"

    # Check that default attributes are still present
    assert "sentry.sdk.name" in attributes_0
    assert "sentry.sdk.version" in attributes_0

    # Test 2: Log with empty custom attributes object
    log_entry_1 = payload["items"][1]
    assert log_entry_1["body"] == "logging with no custom attributes"
    attributes_1 = log_entry_1["attributes"]

    # Should still have default attributes
    assert "sentry.sdk.name" in attributes_1
    assert "sentry.sdk.version" in attributes_1

    # Check that format string parameter was parsed
    assert "sentry.message.parameter.0" in attributes_1
    assert attributes_1["sentry.message.parameter.0"]["value"] == "no"
    assert attributes_1["sentry.message.parameter.0"]["type"] == "string"

    # Test 3: Log with custom attributes that override defaults
    log_entry_2 = payload["items"][2]
    assert log_entry_2["body"] == "logging with a custom parameter attribute"
    attributes_2 = log_entry_2["attributes"]

    # Check custom attribute exists
    assert "sentry.message.parameter.0" in attributes_2
    assert attributes_2["sentry.message.parameter.0"]["value"] == "parameter"
    assert attributes_2["sentry.message.parameter.0"]["type"] == "string"

    # Check that sentry.sdk.name was overwritten by custom attribute
    assert "sentry.sdk.name" in attributes_2
    assert attributes_2["sentry.sdk.name"]["value"] == "custom-sdk-name"
    assert attributes_2["sentry.sdk.name"]["type"] == "string"


def test_logs_global_and_local_attributes_merge(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "set-global-attribute", "log-attributes"],
        env=env,
    )

    assert len(httpserver.log) == 1
    req = httpserver.log[0][0]
    body = req.get_data()

    envelope = Envelope.deserialize(body)

    # Show what the envelope looks like if the test fails
    envelope.print_verbose()

    # Extract the log item
    (log_item,) = envelope.items

    assert log_item.headers["type"] == "log"
    payload = log_item.payload.json

    # We expect 3 log entries based on the log-attributes example
    assert len(payload["items"]) == 3

    log_entry_0 = payload["items"][0]
    attributes_0 = log_entry_0["attributes"]

    # Verify per-log (local) attributes are present

    # passed as global and local attribute, but local should overwrite
    assert "my.custom.attribute" in attributes_0
    assert attributes_0["my.custom.attribute"]["value"] == "my_attribute"
    assert attributes_0["my.custom.attribute"]["type"] == "string"

    assert "global.attribute.bool" in attributes_0
    assert attributes_0["global.attribute.bool"]["value"] == True
    assert attributes_0["global.attribute.bool"]["type"] == "boolean"

    assert "global.attribute.int" in attributes_0
    assert attributes_0["global.attribute.int"]["value"] == 123
    assert attributes_0["global.attribute.int"]["type"] == "integer"

    assert "global.attribute.double" in attributes_0
    assert attributes_0["global.attribute.double"]["value"] == 1.23
    assert attributes_0["global.attribute.double"]["type"] == "double"

    assert "global.attribute.string" in attributes_0
    assert attributes_0["global.attribute.string"]["value"] == "my_global_value"
    assert attributes_0["global.attribute.string"]["type"] == "string"

    assert "global.attribute.array" in attributes_0
    assert attributes_0["global.attribute.array"]["value"] == ["item1", "item2"]
    assert attributes_0["global.attribute.array"]["type"] == "array"
