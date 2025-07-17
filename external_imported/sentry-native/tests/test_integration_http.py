import itertools
import json
import os
import shutil
import sys
import time
import uuid
import subprocess

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
    assert_attachment,
    assert_meta,
    assert_breadcrumb,
    assert_stacktrace,
    assert_event,
    assert_exception,
    assert_inproc_crash,
    assert_session,
    assert_user_feedback,
    assert_minidump,
    assert_breakpad_crash,
    assert_gzip_content_encoding,
    assert_gzip_file_header,
    assert_failed_proxy_auth_request,
    assert_attachment_view_hierarchy,
)
from .conditions import has_http, has_breakpad, has_files, has_crashpad

pytestmark = pytest.mark.skipif(not has_http, reason="tests need http")

# fmt: off
auth_header = (
    "Sentry sentry_key=uiaeosnrtdy, sentry_version=7, sentry_client=sentry.native/0.9.1"
)
# fmt: on


@pytest.mark.parametrize(
    "build_args",
    [
        ({"SENTRY_TRANSPORT_COMPRESSION": "Off"}),
        ({"SENTRY_TRANSPORT_COMPRESSION": "On"}),
    ],
)
def test_capture_http(cmake, httpserver, build_args):
    build_args.update({"SENTRY_BACKEND": "none"})
    tmp_path = cmake(["sentry_example"], build_args)

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "release-env", "capture-event", "add-stacktrace"],
        check=True,
        env=env,
    )

    assert len(httpserver.log) == 1
    req = httpserver.log[0][0]
    body = req.get_data()

    if build_args.get("SENTRY_TRANSPORT_COMPRESSION") == "On":
        assert_gzip_content_encoding(req)
        assert_gzip_file_header(body)

    envelope = Envelope.deserialize(body)

    assert_meta(envelope, "🤮🚀")
    assert_breadcrumb(envelope)
    assert_stacktrace(envelope)

    assert_event(envelope)


def test_session_http(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    # start once without a release, but with a session
    run(
        tmp_path,
        "sentry_example",
        ["log", "release-env", "start-session"],
        check=True,
        env=env,
    )
    run(
        tmp_path,
        "sentry_example",
        ["log", "start-session"],
        check=True,
        env=env,
    )

    assert len(httpserver.log) == 1
    output = httpserver.log[0][0].get_data()
    envelope = Envelope.deserialize(output)

    assert_session(envelope, {"init": True, "status": "exited", "errors": 0})


def test_capture_and_session_http(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "capture-event"],
        check=True,
        env=env,
    )

    assert len(httpserver.log) == 2
    output = httpserver.log[0][0].get_data()
    envelope = Envelope.deserialize(output)

    assert_event(envelope)
    assert_session(envelope, {"init": True, "status": "ok", "errors": 0})

    output = httpserver.log[1][0].get_data()
    envelope = Envelope.deserialize(output)
    assert_session(envelope, {"status": "exited", "errors": 0})


def test_user_feedback_http(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-user-feedback"],
        check=True,
        env=env,
    )

    assert len(httpserver.log) == 2
    output = httpserver.log[0][0].get_data()
    envelope = Envelope.deserialize(output)

    assert_event(envelope, "Hello user feedback!")

    output = httpserver.log[1][0].get_data()
    envelope = Envelope.deserialize(output)

    assert_user_feedback(envelope)


def test_exception_and_session_http(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "capture-exception", "add-stacktrace"],
        check=True,
        env=env,
    )

    assert len(httpserver.log) == 2
    output = httpserver.log[0][0].get_data()
    envelope = Envelope.deserialize(output)

    assert_exception(envelope)
    assert_stacktrace(envelope, inside_exception=True)
    assert_session(envelope, {"init": True, "status": "ok", "errors": 1})

    output = httpserver.log[1][0].get_data()
    envelope = Envelope.deserialize(output)
    assert_session(envelope, {"status": "exited", "errors": 1})


@pytest.mark.skipif(not has_files, reason="test needs a local filesystem")
def test_abnormal_session(cmake, httpserver):
    tmp_path = cmake(
        ["sentry_example"],
        {"SENTRY_BACKEND": "none"},
    )

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    # create a bogus session file
    session = json.dumps(
        {
            "sid": "00000000-0000-0000-0000-000000000000",
            "did": "42",
            "status": "started",
            "errors": 0,
            "started": "2020-06-02T10:04:53.680Z",
            "duration": 10,
            "attrs": {"release": "test-example-release", "environment": "development"},
        }
    )
    db_dir = tmp_path.joinpath(".sentry-native")
    db_dir.mkdir(exist_ok=True)
    # 15 exceeds the max envelope items
    for i in range(15):
        run_dir = db_dir.joinpath(f"foo-{i}.run")
        run_dir.mkdir()
        with open(run_dir.joinpath("session.json"), "w") as session_file:
            session_file.write(session)

    run(
        tmp_path,
        "sentry_example",
        ["log", "no-setup"],
        check=True,
        env=env,
    )

    assert len(httpserver.log) == 2
    envelope1 = Envelope.deserialize(httpserver.log[0][0].get_data())
    envelope2 = Envelope.deserialize(httpserver.log[1][0].get_data())

    session_count = 0
    for item in itertools.chain(envelope1, envelope2):
        if item.headers.get("type") == "session":
            session_count += 1
    assert session_count == 15

    assert_session(envelope1, {"status": "abnormal", "errors": 0, "duration": 10})


@pytest.mark.parametrize(
    "build_args",
    [
        ({"SENTRY_TRANSPORT_COMPRESSION": "Off"}),
        ({"SENTRY_TRANSPORT_COMPRESSION": "On"}),
    ],
)
def test_inproc_crash_http(cmake, httpserver, build_args):
    build_args.update({"SENTRY_BACKEND": "inproc"})
    tmp_path = cmake(["sentry_example"], build_args)

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    child = run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "attachment", "attach-view-hierarchy", "crash"],
        env=env,
    )
    assert child.returncode  # well, it's a crash after all

    run(
        tmp_path,
        "sentry_example",
        ["log", "no-setup"],
        check=True,
        env=env,
    )

    assert len(httpserver.log) == 1
    req = httpserver.log[0][0]
    body = req.get_data()

    if build_args.get("SENTRY_TRANSPORT_COMPRESSION") == "On":
        assert_gzip_content_encoding(req)
        assert_gzip_file_header(body)

    envelope = Envelope.deserialize(body)

    assert_session(envelope, {"init": True, "status": "crashed", "errors": 1})

    assert_meta(envelope, integration="inproc")
    assert_breadcrumb(envelope)
    assert_attachment(envelope)
    assert_attachment_view_hierarchy(envelope)

    assert_inproc_crash(envelope)


def test_inproc_reinstall(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "inproc"})

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))
    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")

    child = run(
        tmp_path,
        "sentry_example",
        ["log", "reinstall", "crash"],
        env=env,
    )
    assert child.returncode  # well, it's a crash after all

    run(
        tmp_path,
        "sentry_example",
        ["log", "no-setup"],
        check=True,
        env=env,
    )

    assert len(httpserver.log) == 1


def test_inproc_dump_inflight(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "inproc"})

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    child = run(
        tmp_path, "sentry_example", ["log", "capture-multiple", "crash"], env=env
    )
    assert child.returncode  # well, it's a crash after all
    run(tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env)

    # we trigger 10 normal events, and 1 crash
    assert len(httpserver.log) >= 11


@pytest.mark.skipif(not has_breakpad, reason="test needs breakpad backend")
@pytest.mark.parametrize(
    "build_args",
    [
        ({"SENTRY_TRANSPORT_COMPRESSION": "Off"}),
        ({"SENTRY_TRANSPORT_COMPRESSION": "On"}),
    ],
)
def test_breakpad_crash_http(cmake, httpserver, build_args):
    build_args.update({"SENTRY_BACKEND": "breakpad"})
    tmp_path = cmake(["sentry_example"], build_args)

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    child = run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "attachment", "attach-view-hierarchy", "crash"],
        env=env,
    )
    assert child.returncode  # well, it's a crash after all

    run(
        tmp_path,
        "sentry_example",
        ["log", "no-setup"],
        check=True,
        env=env,
    )

    assert len(httpserver.log) == 1
    req = httpserver.log[0][0]
    body = req.get_data()

    if build_args.get("SENTRY_TRANSPORT_COMPRESSION") == "On":
        assert_gzip_content_encoding(req)
        assert_gzip_file_header(body)

    envelope = Envelope.deserialize(body)

    assert_session(envelope, {"init": True, "status": "crashed", "errors": 1})

    assert_meta(envelope, integration="breakpad")
    assert_breadcrumb(envelope)
    assert_attachment(envelope)
    assert_attachment_view_hierarchy(envelope)

    assert_breakpad_crash(envelope)
    assert_minidump(envelope)


@pytest.mark.skipif(not has_breakpad, reason="test needs breakpad backend")
def test_breakpad_reinstall(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "breakpad"})

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))
    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")

    child = run(
        tmp_path,
        "sentry_example",
        ["log", "reinstall", "crash"],
        env=env,
    )
    assert child.returncode  # well, it's a crash after all

    run(
        tmp_path,
        "sentry_example",
        ["log", "no-setup"],
        check=True,
        env=env,
    )

    assert len(httpserver.log) == 1


@pytest.mark.skipif(not has_breakpad, reason="test needs breakpad backend")
def test_breakpad_dump_inflight(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "breakpad"})

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    child = run(
        tmp_path, "sentry_example", ["log", "capture-multiple", "crash"], env=env
    )
    assert child.returncode  # well, it's a crash after all

    run(tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env)

    # we trigger 10 normal events, and 1 crash
    assert len(httpserver.log) >= 11


def test_shutdown_timeout(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    # the timings here are:
    # * the process waits 2s for the background thread to shut down, which fails
    # * it then dumps everything and waits another 1s before terminating the process
    # * the python runner waits for 2.4s in total to close the request, which
    #   will cleanly terminate the background worker.
    # the assumption here is that 2s < 2.4s < 2s+1s. but since those timers
    # run in different processes, this has the potential of being flaky

    def delayed(req):
        time.sleep(2.4)
        return "{}"

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_handler(delayed)
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    # Using `sleep-after-shutdown` here means that the background worker will
    # deref/free itself, so we will not leak in that case!
    child = run(
        tmp_path,
        "sentry_example",
        ["log", "capture-multiple", "sleep-after-shutdown"],
        env=env,
        check=True,
    )
    assert child.returncode == 0

    httpserver.clear_all_handlers()
    httpserver.clear_log()

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")

    run(tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env)

    assert len(httpserver.log) == 10


RFC3339_FORMAT = "%Y-%m-%dT%H:%M:%S.%fZ"


@pytest.mark.parametrize(
    "build_args",
    [
        ({"SENTRY_TRANSPORT_COMPRESSION": "Off"}),
        ({"SENTRY_TRANSPORT_COMPRESSION": "On"}),
    ],
)
def test_transaction_only(cmake, httpserver, build_args):
    build_args.update({"SENTRY_BACKEND": "none"})
    tmp_path = cmake(["sentry_example"], build_args)

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-transaction"],
        check=True,
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

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-transaction", "before-transaction"],
        check=True,
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

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-transaction", "discarding-before-transaction"],
        check=True,
        env=env,
    )

    # transaction should have been discarded
    assert len(httpserver.log) == 0


def test_transaction_event(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-transaction", "capture-event"],
        check=True,
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
    # non-scoped tx should differ in span_id from the event span_id
    assert trace_context["span_id"]
    assert (
        trace_context["span_id"] != event.payload.json["contexts"]["trace"]["span_id"]
    )


def test_set_trace_event(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "set-trace", "capture-event"],
        check=True,
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

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver), SENTRY_RELEASE="🤮🚀")

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-transaction", "scope-transaction-event"],
        check=True,
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

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
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
        check=True,
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


def test_capture_minidump(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    # make sure we are isolated from previous runs
    shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")

    run(
        tmp_path,
        "sentry_example",
        ["log", "attachment", "attach-view-hierarchy", "capture-minidump"],
        check=True,
        env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
    )

    assert len(httpserver.log) == 1

    req = httpserver.log[0][0]
    body = req.get_data()

    envelope = Envelope.deserialize(body)

    assert_breadcrumb(envelope)
    assert_attachment(envelope)
    assert_attachment_view_hierarchy(envelope)

    assert_minidump(envelope)


def _setup_http_proxy_test(cmake, httpserver, proxy, proxy_auth=None):
    proxy_process = start_mitmdump(proxy, proxy_auth) if proxy else None

    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    # make sure we are isolated from previous runs
    shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver, proxy_host=True))
    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")

    return env, proxy_process, tmp_path


def test_proxy_from_env(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    setup_proxy_env_vars(port=8080)
    try:
        env, proxy_process, tmp_path = _setup_http_proxy_test(
            cmake, httpserver, "http-proxy"
        )

        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-event"],
            check=True,
            env=env,
        )

    finally:
        cleanup_proxy_env_vars()
        proxy_test_finally(1, httpserver, proxy_process)


def test_proxy_from_env_port_incorrect(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    setup_proxy_env_vars(port=8081)
    try:
        env, proxy_process, tmp_path = _setup_http_proxy_test(
            cmake, httpserver, "http-proxy"
        )

        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-event"],
            check=True,
            env=env,
        )

    finally:
        cleanup_proxy_env_vars()
        proxy_test_finally(0, httpserver, proxy_process)


def test_proxy_auth(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    try:
        env, proxy_process, tmp_path = _setup_http_proxy_test(
            cmake, httpserver, "http-proxy", proxy_auth="user:password"
        )

        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-event", "http-proxy-auth"],
            check=True,
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver, proxy_host=True)),
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
        env, proxy_process, tmp_path = _setup_http_proxy_test(
            cmake, httpserver, "http-proxy", proxy_auth="wrong:wrong"
        )

        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-event", "http-proxy-auth"],
            check=True,
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver, proxy_host=True)),
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
        env, proxy_process, tmp_path = _setup_http_proxy_test(
            cmake, httpserver, "http-proxy"
        )

        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-event", "http-proxy-ipv6"],
            check=True,
            env=env,
        )

    finally:
        proxy_test_finally(1, httpserver, proxy_process)


def test_proxy_set_empty(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    setup_proxy_env_vars(port=8080)  # we start the proxy but expect it to remain unused
    try:
        env, proxy_process, tmp_path = _setup_http_proxy_test(
            cmake, httpserver, "http-proxy"
        )

        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-event", "proxy-empty"],
            check=True,
            env=env,
        )

    finally:
        cleanup_proxy_env_vars()
        proxy_test_finally(1, httpserver, proxy_process, expected_proxy_logsize=0)


def test_proxy_https_not_http(cmake, httpserver):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    # we start the proxy but expect it to remain unused (dsn is http, so shouldn't use https proxy)
    os.environ["https_proxy"] = f"http://localhost:8080"
    try:
        env, proxy_process, tmp_path = _setup_http_proxy_test(
            cmake, httpserver, "http-proxy"
        )

        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-event"],
            check=True,
            env=env,
        )

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
def test_capture_proxy(cmake, httpserver, run_args, proxy_running):
    if not shutil.which("mitmdump"):
        pytest.skip("mitmdump is not installed")

    proxy_process = None  # store the proxy process to terminate it later
    expected_logsize = 0

    try:
        proxy_to_start = run_args[0] if proxy_running else None
        env, proxy_process, tmp_path = _setup_http_proxy_test(
            cmake, httpserver, proxy_to_start
        )
        run(
            tmp_path,
            "sentry_example",
            ["log", "capture-event"]
            + run_args,  # only passes if given proxy is running
            check=True,
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver, proxy_host=True)),
        )
        if proxy_running:
            expected_logsize = 1
        else:
            expected_logsize = 0
    finally:
        proxy_test_finally(expected_logsize, httpserver, proxy_process)


def test_capture_with_scope(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    # make sure we are isolated from previous runs
    shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")

    run(
        tmp_path,
        "sentry_example",
        ["log", "attach-to-scope", "capture-with-scope"],
        check=True,
        env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
    )

    assert len(httpserver.log) == 1

    req = httpserver.log[0][0]
    body = req.get_data()

    envelope = Envelope.deserialize(body)

    assert_breadcrumb(envelope, "scoped crumb")
    assert_attachment(envelope)
