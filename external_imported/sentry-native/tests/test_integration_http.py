import pytest
import subprocess
import sys
import os
import time
import itertools
import json
from . import make_dsn, check_output, run, Envelope
from .conditions import has_http, has_breakpad, has_files
from .assertions import (
    assert_attachment,
    assert_meta,
    assert_breadcrumb,
    assert_stacktrace,
    assert_event,
    assert_exception,
    assert_crash,
    assert_session,
    assert_minidump,
)

pytestmark = pytest.mark.skipif(not has_http, reason="tests need http")

auth_header = (
    "Sentry sentry_key=uiaeosnrtdy, sentry_version=7, sentry_client=sentry.native/0.4.8"
)


def test_capture_http(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/", headers={"x-sentry-auth": auth_header},
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
    output = httpserver.log[0][0].get_data()
    envelope = Envelope.deserialize(output)

    assert_meta(envelope, "🤮🚀")
    assert_breadcrumb(envelope)
    assert_stacktrace(envelope)

    assert_event(envelope)


def test_session_http(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request(
        "/api/123456/envelope/", headers={"x-sentry-auth": auth_header},
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
        tmp_path, "sentry_example", ["log", "start-session"], check=True, env=env,
    )

    assert len(httpserver.log) == 1
    output = httpserver.log[0][0].get_data()
    envelope = Envelope.deserialize(output)

    assert_session(envelope, {"init": True, "status": "exited", "errors": 0})


def test_capture_and_session_http(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request(
        "/api/123456/envelope/", headers={"x-sentry-auth": auth_header},
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


def test_exception_and_session_http(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request(
        "/api/123456/envelope/", headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "capture-exception"],
        check=True,
        env=env,
    )

    assert len(httpserver.log) == 2
    output = httpserver.log[0][0].get_data()
    envelope = Envelope.deserialize(output)

    assert_exception(envelope)
    assert_session(envelope, {"init": True, "status": "ok", "errors": 1})

    output = httpserver.log[1][0].get_data()
    envelope = Envelope.deserialize(output)
    assert_session(envelope, {"status": "exited", "errors": 1})


@pytest.mark.skipif(not has_files, reason="test needs a local filesystem")
def test_abnormal_session(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"},)

    httpserver.expect_request(
        "/api/123456/envelope/", headers={"x-sentry-auth": auth_header},
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
        tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env,
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


def test_inproc_crash_http(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "inproc"})

    httpserver.expect_request(
        "/api/123456/envelope/", headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    child = run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "attachment", "crash"],
        env=env,
    )
    assert child.returncode  # well, its a crash after all

    run(
        tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env,
    )

    assert len(httpserver.log) == 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())

    assert_session(envelope, {"init": True, "status": "crashed", "errors": 1})

    assert_meta(envelope, integration="inproc")
    assert_breadcrumb(envelope)
    assert_attachment(envelope)

    assert_crash(envelope)


def test_inproc_dump_inflight(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "inproc"})

    httpserver.expect_request(
        "/api/123456/envelope/", headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    child = run(
        tmp_path, "sentry_example", ["log", "capture-multiple", "crash"], env=env
    )
    assert child.returncode  # well, its a crash after all

    run(tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env)

    # we trigger 10 normal events, and 1 crash
    assert len(httpserver.log) >= 11


@pytest.mark.skipif(not has_breakpad, reason="test needs breakpad backend")
def test_breakpad_crash_http(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "breakpad"})

    httpserver.expect_request(
        "/api/123456/envelope/", headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    child = run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "attachment", "crash"],
        env=env,
    )
    assert child.returncode  # well, its a crash after all

    run(
        tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env,
    )

    assert len(httpserver.log) == 1
    envelope = Envelope.deserialize(httpserver.log[0][0].get_data())

    assert_session(envelope, {"init": True, "status": "crashed", "errors": 1})

    assert_meta(envelope, integration="breakpad")
    assert_breadcrumb(envelope)
    assert_attachment(envelope)

    assert_minidump(envelope)


@pytest.mark.skipif(not has_breakpad, reason="test needs breakpad backend")
def test_breakpad_dump_inflight(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "breakpad"})

    httpserver.expect_request(
        "/api/123456/envelope/", headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    child = run(
        tmp_path, "sentry_example", ["log", "capture-multiple", "crash"], env=env
    )
    assert child.returncode  # well, its a crash after all

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
        "/api/123456/envelope/", headers={"x-sentry-auth": auth_header},
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

    httpserver.clear_all_handlers()
    httpserver.clear_log()

    httpserver.expect_request(
        "/api/123456/envelope/", headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")

    run(tmp_path, "sentry_example", ["log", "no-setup"], check=True, env=env)

    assert len(httpserver.log) == 10
