import itertools
import json
import os
import subprocess
import time

import pytest
from flaky import flaky

from . import (
    make_dsn,
    run,
    stage_replay,
    Envelope,
    split_log_request_cond,
    extract_request,
    is_feedback_envelope,
    is_replay_envelope,
    REPLAY_ID,
    SENTRY_VERSION,
)
from .assertions import (
    assert_attachment,
    assert_meta,
    assert_breadcrumb,
    assert_stacktrace,
    assert_event,
    assert_exception,
    assert_inproc_crash,
    assert_replay_envelope,
    assert_session,
    assert_user_feedback,
    assert_user_report,
    assert_minidump,
    assert_breakpad_crash,
    assert_gzip_content_encoding,
    assert_gzip_file_header,
    assert_attachment_view_hierarchy,
    assert_before_breadcrumb,
    assert_no_breadcrumbs,
)
from .conditions import (
    has_http,
    has_breakpad,
    has_native,
    has_files,
    is_asan,
    is_qemu,
)


def get_asan_crash_env(env):
    """
    Configure ASAN options for crash testing.
    Disables ASAN's signal handling so our crash handler can run.
    """
    if not is_asan:
        return env
    # Preserve existing ASAN_OPTIONS and add signal handling overrides
    asan_opts = env.get("ASAN_OPTIONS", "")
    # Disable handling of crash signals so our handler can run
    asan_signal_opts = (
        "handle_segv=0:handle_sigbus=0:handle_abort=0:"
        "handle_sigfpe=0:handle_sigill=0:allow_user_segv_handler=1"
    )
    if asan_opts:
        env = dict(env, ASAN_OPTIONS=f"{asan_opts}:{asan_signal_opts}")
    else:
        env = dict(env, ASAN_OPTIONS=asan_signal_opts)
    return env


pytestmark = pytest.mark.skipif(not has_http, reason="tests need http")


# fmt: off
auth_header = (
    f"Sentry sentry_key=uiaeosnrtdy, sentry_version=7, sentry_client=sentry.native/{SENTRY_VERSION}"
)
# fmt: on


@pytest.mark.skipif(is_qemu, reason="unreliable under qemu-user")
@pytest.mark.parametrize(
    "build_args",
    [
        ({}),  # SENTRY_TRANSPORT_COMPRESSION=Off (default, cached)
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
        env=env,
    )
    run(
        tmp_path,
        "sentry_example",
        ["log", "start-session"],
        env=env,
    )

    assert len(httpserver.log) == 1
    output = httpserver.log[0][0].get_data()
    envelope = Envelope.deserialize(output)

    assert_session(envelope, {"init": True, "status": "exited", "errors": 0})


def test_set_release_and_environment_http(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "update-release-env", "start-session", "capture-event"],
        env=env,
    )

    assert len(httpserver.log) == 2

    output = httpserver.log[0][0].get_data()
    envelope = Envelope.deserialize(output)
    event = envelope.get_event()
    assert event["release"] == "updated-release"
    assert event["environment"] == "updated-environment"
    assert_session(
        envelope,
        {"init": True, "status": "ok", "errors": 0},
        release="updated-release",
        environment="updated-environment",
    )

    output = httpserver.log[1][0].get_data()
    envelope = Envelope.deserialize(output)
    assert_session(
        envelope,
        {"status": "exited", "errors": 0},
        release="updated-release",
        environment="updated-environment",
    )


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
        env=env,
    )

    assert len(httpserver.log) == 1
    output = httpserver.log[0][0].get_data()
    envelope = Envelope.deserialize(output)

    assert_user_feedback(envelope)


def test_user_feedback_with_attachments_http(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-user-feedback-with-attachment"],
        env=env,
    )

    assert len(httpserver.log) == 1
    output = httpserver.log[0][0].get_data()
    envelope = Envelope.deserialize(output)

    # Verify the feedback is present
    assert_user_feedback(envelope)

    # Verify attachments are present
    attachment_count = 0
    for item in envelope:
        if item.headers.get("type") == "attachment":
            attachment_count += 1

    # Should have 2 attachments (one file, one bytes)
    assert attachment_count == 2


def test_user_report_http(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-user-report"],
        env=env,
    )

    assert len(httpserver.log) == 2
    output = httpserver.log[0][0].get_data()
    envelope = Envelope.deserialize(output)

    assert_event(envelope, "Hello user feedback!")

    output = httpserver.log[1][0].get_data()
    envelope = Envelope.deserialize(output)

    assert_user_report(envelope)


@pytest.mark.parametrize(
    "build_args",
    [
        ({"SENTRY_BACKEND": "inproc"}),
        pytest.param(
            {"SENTRY_BACKEND": "breakpad"},
            marks=pytest.mark.skipif(
                not has_breakpad or is_qemu, reason="test needs breakpad backend"
            ),
        ),
    ],
)
def test_external_crash_reporter_http(cmake, httpserver, build_args):
    tmp_path = cmake(["sentry_example", "sentry_crash_reporter"], build_args)
    cache_dir = tmp_path.joinpath(".sentry-native/cache")

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))
        run(
            tmp_path,
            "sentry_example",
            ["log", "crash-reporter", "cache-keep", "crash"],
            expect_failure=True,
            env=env,
        )

        # the session crash heuristic on Mac uses timestamps, so make sure we have
        # a small delay here
        time.sleep(1)

        run(
            tmp_path,
            "sentry_example",
            ["log", "no-setup"],
            env=env,
        )
    assert waiting.result

    assert len(httpserver.log) == 2
    feedback_request, crash_request = split_log_request_cond(
        httpserver.log, is_feedback_envelope
    )
    feedback = feedback_request.get_data()
    crash = crash_request.get_data()

    envelope = Envelope.deserialize(crash)
    assert envelope.headers["cache_dir"] == str(cache_dir)
    assert_meta(envelope, integration=build_args.get("SENTRY_BACKEND", ""))

    envelope = Envelope.deserialize(feedback)
    assert_user_feedback(envelope)


@pytest.mark.skipif(is_qemu, reason="unreliable under qemu-user")
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
            "did": "11111111-1111-1111-1111-111111111111",
            "status": "started",
            "errors": 0,
            "started": "2020-06-02T10:04:53.680Z",
            "duration": 10,
            "attrs": {"release": "test-example-release", "environment": "development"},
        }
    )
    db_dir = tmp_path.joinpath(".sentry-native")
    db_dir.mkdir(exist_ok=True)
    # 101 exceeds the max session items
    for i in range(101):
        run_dir = db_dir.joinpath(f"foo-{i}.run")
        run_dir.mkdir()
        with open(run_dir.joinpath("session.json"), "w") as session_file:
            session_file.write(session)

    run(
        tmp_path,
        "sentry_example",
        ["log", "no-setup"],
        env=env,
    )

    assert len(httpserver.log) == 2
    envelope1 = Envelope.deserialize(httpserver.log[0][0].get_data())
    envelope2 = Envelope.deserialize(httpserver.log[1][0].get_data())

    session_count = 0
    for item in itertools.chain(envelope1, envelope2):
        if item.headers.get("type") == "session":
            session_count += 1
    assert session_count == 101

    assert_session(envelope1, {"status": "abnormal", "errors": 0, "duration": 10})


@pytest.mark.parametrize(
    "build_args",
    [
        ({}),  # SENTRY_TRANSPORT_COMPRESSION=Off (default, cached)
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

    run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "attachment", "attach-view-hierarchy", "crash"],
        expect_failure=True,
        env=env,
    )

    run(
        tmp_path,
        "sentry_example",
        ["log", "no-setup"],
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

    run(
        tmp_path,
        "sentry_example",
        ["log", "reinstall", "crash"],
        expect_failure=True,
        env=env,
    )

    run(
        tmp_path,
        "sentry_example",
        ["log", "no-setup"],
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

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-multiple", "crash"],
        expect_failure=True,
        env=env,
    )
    run(tmp_path, "sentry_example", ["log", "no-setup"], env=env)

    # we trigger 10 normal events, and 1 crash
    assert len(httpserver.log) >= 11


@pytest.mark.skipif(not has_breakpad or is_qemu, reason="test needs breakpad backend")
@pytest.mark.parametrize(
    "build_args",
    [
        ({}),  # SENTRY_TRANSPORT_COMPRESSION=Off (default, cached)
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

    run(
        tmp_path,
        "sentry_example",
        ["log", "start-session", "attachment", "attach-view-hierarchy", "crash"],
        expect_failure=True,
        env=env,
    )

    run(
        tmp_path,
        "sentry_example",
        ["log", "no-setup"],
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


@pytest.mark.parametrize(
    "backend",
    [
        "inproc",
        pytest.param(
            "breakpad",
            marks=pytest.mark.skipif(
                not has_breakpad or is_qemu, reason="test needs breakpad backend"
            ),
        ),
    ],
)
def test_crash_replay_envelope_http(cmake, httpserver, backend):
    """A staged replay referenced by the crash event's `contexts.replay` is
    consumed at crash time, persisted to the run folder as its own
    `replay_video` envelope, and sent on the next launch."""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": backend})

    replays, video = stage_replay(tmp_path)

    httpserver.expect_request("/api/123456/envelope/").respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "replay-context", "crash"],
        expect_failure=True,
        env=env,
    )

    # the staged replay is consumed at crash time
    assert not (replays / f"replay-{REPLAY_ID}.mp4").exists()
    assert not (replays / f"replay-{REPLAY_ID}.json").exists()

    run(
        tmp_path,
        "sentry_example",
        ["log", "no-setup"],
        env=env,
    )

    # crash envelope + replay envelope
    assert len(httpserver.log) == 2
    replay_request, others = extract_request(httpserver.log, is_replay_envelope)
    assert replay_request is not None, "expected a replay_video envelope"
    assert_replay_envelope(Envelope.deserialize(replay_request.get_data()), video)

    crash_envelope = Envelope.deserialize(others[0][0].get_data())
    event = crash_envelope.get_event()
    assert event["contexts"]["replay"]["replay_id"] == REPLAY_ID


@pytest.mark.skipif(not has_breakpad or is_qemu, reason="test needs breakpad backend")
def test_breakpad_reinstall(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "breakpad"})

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))
    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")

    run(
        tmp_path,
        "sentry_example",
        ["log", "reinstall", "crash"],
        expect_failure=True,
        env=env,
    )

    run(
        tmp_path,
        "sentry_example",
        ["log", "no-setup"],
        env=env,
    )

    assert len(httpserver.log) == 1


@pytest.mark.skipif(not has_breakpad or is_qemu, reason="test needs breakpad backend")
@flaky(max_runs=3)
def test_breakpad_dump_inflight(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "breakpad"})

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-multiple", "crash"],
        expect_failure=True,
        env=env,
        timeout=300,
    )

    run(tmp_path, "sentry_example", ["log", "no-setup"], env=env)

    # we trigger 10 normal events, and 1 crash
    assert len(httpserver.log) >= 11


@flaky(max_runs=3)
def test_shutdown_timeout(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    # the timings here are:
    # * the process waits 2s for the background thread to shut down, which fails
    # * it then dumps everything and waits another 1s before terminating the process
    # * the python runner waits for 2.5s in total to close the request, which
    #   will cleanly terminate the background worker.
    # the assumption here is that 2s < 2.5s < 2s+1s. The margins are tight
    # (0.5s on each side), so in CI environments with load this can still be
    # flaky. We use >= instead of == to tolerate minor timing variations.

    def delayed(req):
        time.sleep(2.5)
        return "{}"

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_handler(delayed)
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    # Using `sleep-after-shutdown` here means that the background worker will
    # deref/free itself, so we will not leak in that case!
    run(
        tmp_path,
        "sentry_example",
        ["log", "capture-multiple", "sleep-after-shutdown"],
        env=env,
    )

    httpserver.clear_all_handlers()
    httpserver.clear_log()

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")

    run(tmp_path, "sentry_example", ["log", "no-setup"], env=env)

    # The test verifies that events are properly dumped to disk when shutdown
    # times out and sent on restart. Due to timing variations across platforms
    # and CI environments, not all 10 events may make it through. We require
    # at least 3 to verify the core functionality works (dump to disk + send
    # on restart). The exact count depends on how many events were queued
    # before the shutdown timeout kicked in.
    assert len(httpserver.log) >= 3, (
        f"Expected at least 3 events to be sent on restart, got {len(httpserver.log)}. "
        "Events should be dumped to disk on shutdown timeout and sent on restart."
    )


def test_capture_minidump(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")

    run(
        tmp_path,
        "sentry_example",
        ["log", "attachment", "attach-view-hierarchy", "capture-minidump"],
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


def test_capture_with_scope(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")

    run(
        tmp_path,
        "sentry_example",
        ["log", "attach-to-scope", "capture-with-scope"],
        env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
    )

    assert len(httpserver.log) == 1

    req = httpserver.log[0][0]
    body = req.get_data()

    envelope = Envelope.deserialize(body)

    assert_breadcrumb(envelope, "scoped crumb")
    assert_attachment(envelope)


def test_before_breadcrumb_http(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "before-breadcrumb", "capture-event"],
        env=env,
    )

    assert len(httpserver.log) == 1
    output = httpserver.log[0][0].get_data()
    envelope = Envelope.deserialize(output)

    assert_event(envelope)
    assert_before_breadcrumb(envelope)


def test_discarding_before_breadcrumb_http(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    run(
        tmp_path,
        "sentry_example",
        ["log", "discarding-before-breadcrumb", "capture-event"],
        env=env,
    )

    assert len(httpserver.log) == 1
    output = httpserver.log[0][0].get_data()
    envelope = Envelope.deserialize(output)

    assert_event(envelope)
    assert_no_breadcrumbs(envelope)


@pytest.mark.skipif(not has_native or is_qemu, reason="test needs native backend")
def test_native_crash_http(cmake, httpserver):
    """Test native backend crash handling with HTTP transport"""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "native"})

    httpserver.expect_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    # Use stdout for initialization delay under TSAN
    # Configure ASAN to not intercept crash signals
    run(
        tmp_path,
        "sentry_example",
        ["log", "stdout", "attachment", "crash"],
        expect_failure=True,
        env=get_asan_crash_env(env),
    )

    # Wait for crash to be processed (longer delay for TSAN)
    time.sleep(2)

    # Restart to send the crash
    run(
        tmp_path,
        "sentry_example",
        ["log", "no-setup"],
        env=env,
    )

    assert len(httpserver.log) >= 1
    req = httpserver.log[0][0]
    envelope = Envelope.deserialize(req.get_data())

    assert_minidump(envelope)
    assert_breadcrumb(envelope)
    assert_attachment(envelope)


@pytest.mark.parametrize(
    "backend",
    [
        "inproc",
        pytest.param(
            "breakpad",
            marks=pytest.mark.skipif(
                not has_breakpad or is_qemu, reason="test needs breakpad backend"
            ),
        ),
        pytest.param(
            "native",
            marks=pytest.mark.skipif(
                not has_native or is_qemu,
                reason="test needs native backend",
            ),
        ),
    ],
)
def test_trace_finish_on_crash(cmake, httpserver, backend):
    """The backend's crash handler calls `sentry__trace_finish`, so an
    unfinished transaction on the scope ships alongside the crash."""
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": backend})

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    with httpserver.wait(timeout=10) as waiting:
        run(
            tmp_path,
            "sentry_example",
            ["log", "open-transaction", "crash"],
            expect_failure=True,
            env=env,
        )
        if backend != "native":
            # inproc/breakpad cache to disk; the next launch ships them.
            run(tmp_path, "sentry_example", ["log", "no-setup"], env=env)
    assert waiting.result

    envelopes = [Envelope.deserialize(req.get_data()) for req, _ in httpserver.log]
    for envelope in envelopes:
        item_types = [item.headers.get("type") for item in envelope.items]
        assert not ("event" in item_types and "transaction" in item_types)

    tx_envelopes = [
        envelope
        for envelope in envelopes
        if any(item.headers.get("type") == "transaction" for item in envelope.items)
    ]
    assert tx_envelopes
    tx_envelope = tx_envelopes[0]
    tx_items = [
        item for item in tx_envelope.items if item.headers.get("type") == "transaction"
    ]
    assert tx_items

    tx = tx_items[0].payload.json
    assert tx_envelope.headers["event_id"] == tx["event_id"]
    tx_trace = tx["contexts"]["trace"]
    assert tx_trace["status"] == "aborted"
    assert "parent_span_id" not in tx_trace
    tx_trace_header = tx_envelope.headers["trace"]
    assert tx_trace_header["trace_id"] == tx_trace["trace_id"]
    assert tx_trace_header["sampled"] == "true"
    assert tx_trace_header["transaction"] == tx["transaction"]
    spans = tx.get("spans", [])
    child = next(s for s in spans if s.get("op") == "open.span")
    grand = next(s for s in spans if s.get("op") == "open.grand.span")
    assert child["parent_span_id"] == tx_trace["span_id"]
    assert grand["parent_span_id"] == child["span_id"]
    # Every in-flight child is finished, not just the deepest.
    for op in ("open.span", "open.grand.span"):
        span = next((s for s in spans if s.get("op") == op), None)
        assert span is not None, f"missing {op} in {[s.get('op') for s in spans]}"
        assert span.get("status") == "aborted"
        assert span.get("timestamp")

    # The crash event nests under the deepest active span via matching
    # trace_id + span_id.
    event_envelopes = [
        envelope
        for envelope in envelopes
        if any(item.headers.get("type") == "event" for item in envelope.items)
    ]
    assert event_envelopes
    event_envelope = event_envelopes[0]
    event_items = [
        item for item in event_envelope.items if item.headers.get("type") == "event"
    ]
    assert event_items
    if backend != "native":
        event_trace_header = event_envelope.headers["trace"]
        assert event_trace_header["trace_id"] == tx_trace["trace_id"]
        assert event_trace_header["sampled"] == "true"
    event = event_items[0].payload.json
    assert event["contexts"]["trace"]["trace_id"] == tx_trace["trace_id"]
    assert event["contexts"]["trace"]["span_id"] == grand["span_id"]
    assert event.get("level") == "fatal"


@pytest.mark.skipif(not has_files, reason="test needs a local filesystem")
def test_http_retry_on_network_error(cmake, httpserver, unreachable_dsn):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "inproc"})
    cache_dir = tmp_path.joinpath(".sentry-native/cache")

    # unreachable port triggers CURLE_COULDNT_CONNECT
    run(
        tmp_path,
        "sentry_example",
        ["log", "http-retry", "capture-event"],
        env=dict(os.environ, SENTRY_DSN=unreachable_dsn),
    )

    assert cache_dir.exists()
    cache_files = list(cache_dir.glob("*.envelope"))
    assert len(cache_files) == 1
    assert "-00-" in str(cache_files[0].name)
    envelope_uuid = cache_files[0].stem[-36:]

    # retry on next run with working server
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
    assert envelope.headers["event_id"] == envelope_uuid
    assert_meta(envelope, integration="inproc")

    cache_files = list(cache_dir.glob("*.envelope"))
    assert len(cache_files) == 0


@pytest.mark.skipif(not has_files, reason="test needs a local filesystem")
def test_http_retry_multiple_attempts(cmake, httpserver, unreachable_dsn):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "inproc"})
    cache_dir = tmp_path.joinpath(".sentry-native/cache")

    env = dict(os.environ, SENTRY_DSN=unreachable_dsn)

    run(tmp_path, "sentry_example", ["log", "http-retry", "capture-event"], env=env)

    cache_files = list(cache_dir.glob("*.envelope"))
    assert len(cache_files) == 1
    assert "-00-" in str(cache_files[0].name)
    envelope_uuid = cache_files[0].stem[-36:]
    envelope = Envelope.deserialize(cache_files[0].read_bytes())
    assert envelope.headers["event_id"] == envelope_uuid

    run(tmp_path, "sentry_example", ["log", "http-retry", "no-setup"], env=env)

    cache_files = list(cache_dir.glob("*.envelope"))
    assert len(cache_files) == 1
    assert "-01-" in str(cache_files[0].name)
    assert cache_files[0].stem[-36:] == envelope_uuid

    run(tmp_path, "sentry_example", ["log", "http-retry", "no-setup"], env=env)

    cache_files = list(cache_dir.glob("*.envelope"))
    assert len(cache_files) == 1
    assert "-02-" in str(cache_files[0].name)
    assert cache_files[0].stem[-36:] == envelope_uuid

    # exhaust remaining retries (max 6)
    for i in range(4):
        run(tmp_path, "sentry_example", ["log", "http-retry", "no-setup"], env=env)

    # discarded after max retries (cache_keep not enabled)
    cache_files = list(cache_dir.glob("*.envelope"))
    assert len(cache_files) == 0


@pytest.mark.skipif(not has_files, reason="test needs a local filesystem")
def test_http_retry_with_cache_keep(cmake, httpserver, unreachable_dsn):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "inproc"})
    cache_dir = tmp_path.joinpath(".sentry-native/cache")

    run(
        tmp_path,
        "sentry_example",
        ["log", "http-retry", "cache-keep", "capture-event"],
        env=dict(os.environ, SENTRY_DSN=unreachable_dsn),
    )

    assert cache_dir.exists()
    assert len(list(cache_dir.glob("*.envelope"))) == 1

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        run(
            tmp_path,
            "sentry_example",
            ["log", "http-retry", "cache-keep", "no-setup"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    assert len(list(cache_dir.glob("*.envelope"))) == 0


@pytest.mark.skipif(not has_files, reason="test needs a local filesystem")
def test_http_retry_cache_keep_max_attempts(cmake, httpserver, unreachable_dsn):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})
    cache_dir = tmp_path.joinpath(".sentry-native/cache")

    env = dict(os.environ, SENTRY_DSN=unreachable_dsn)

    run(
        tmp_path,
        "sentry_example",
        ["log", "http-retry", "cache-keep", "capture-event"],
        env=env,
    )

    assert cache_dir.exists()
    assert len(list(cache_dir.glob("*.envelope"))) == 1

    for _ in range(5):
        run(
            tmp_path,
            "sentry_example",
            ["log", "http-retry", "cache-keep", "no-setup"],
            env=env,
        )

    assert cache_dir.exists()
    assert len(list(cache_dir.glob("*.envelope"))) == 1

    # last attempt succeeds — envelope should be removed, not cached
    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        run(
            tmp_path,
            "sentry_example",
            ["log", "http-retry", "cache-keep", "no-setup"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    assert len(list(cache_dir.glob("*.envelope"))) == 0


@pytest.mark.skipif(not has_files, reason="test needs a local filesystem")
def test_http_retry_http_error_discards_envelope(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "inproc"})
    cache_dir = tmp_path.joinpath(".sentry-native/cache")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data(
        "Internal Server Error", status=500
    )

    with httpserver.wait(timeout=10) as waiting:
        run(tmp_path, "sentry_example", ["log", "http-retry", "capture-event"], env=env)
    assert waiting.result

    # HTTP errors discard, not retry
    cache_files = list(cache_dir.glob("*.envelope")) if cache_dir.exists() else []
    assert len(cache_files) == 0


@pytest.mark.skipif(not has_files, reason="test needs a local filesystem")
def test_http_retry_rate_limit_discards_envelope(cmake, httpserver):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "inproc"})
    cache_dir = tmp_path.joinpath(".sentry-native/cache")
    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))

    httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data(
        "Rate Limited", status=429, headers={"retry-after": "60"}
    )

    with httpserver.wait(timeout=10) as waiting:
        run(tmp_path, "sentry_example", ["log", "http-retry", "capture-event"], env=env)
    assert waiting.result

    # 429 discards, not retry
    cache_files = list(cache_dir.glob("*.envelope")) if cache_dir.exists() else []
    assert len(cache_files) == 0


@pytest.mark.skipif(not has_files, reason="test needs a local filesystem")
def test_http_retry_multiple_success(cmake, httpserver, unreachable_dsn):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})
    db_dir = tmp_path.joinpath(".sentry-native")
    cache_dir = db_dir.joinpath("cache")

    run(
        tmp_path,
        "sentry_example",
        ["log", "http-retry", "capture-multiple"],
        env=dict(os.environ, SENTRY_DSN=unreachable_dsn),
    )

    # envelopes end up in cache/ (retry) or *.run/ (dumped on shutdown timeout)
    cached = list(cache_dir.glob("*.envelope"))
    dumped = list(db_dir.glob("*.run/*.envelope"))
    assert len(cached) + len(dumped) == 10

    for _ in range(10):
        httpserver.expect_oneshot_request("/api/123456/envelope/").respond_with_data(
            "OK"
        )

    with httpserver.wait(timeout=10) as waiting:
        run(
            tmp_path,
            "sentry_example",
            ["log", "http-retry", "no-setup"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )
    assert waiting.result

    assert len(httpserver.log) == 10
    cache_files = list(cache_dir.glob("*.envelope"))
    assert len(cache_files) == 0


@pytest.mark.skipif(not has_files, reason="test needs a local filesystem")
def test_http_retry_multiple_network_error(cmake, unreachable_dsn):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})
    db_dir = tmp_path.joinpath(".sentry-native")
    cache_dir = db_dir.joinpath("cache")

    env = dict(os.environ, SENTRY_DSN=unreachable_dsn)

    run(
        tmp_path,
        "sentry_example",
        ["log", "http-retry", "capture-multiple"],
        env=env,
    )

    # envelopes end up in cache/ (retry) or *.run/ (dumped on shutdown timeout)
    cached = list(cache_dir.glob("*.envelope"))
    dumped = list(db_dir.glob("*.run/*.envelope"))
    assert len(cached) + len(dumped) == 10

    run(
        tmp_path,
        "sentry_example",
        ["log", "http-retry", "no-setup"],
        env=env,
    )

    # envelopes end up in cache/ (retry) or *.run/ (dumped on shutdown timeout)
    cached = list(cache_dir.glob("*.envelope"))
    dumped = list(db_dir.glob("*.run/*.envelope"))
    assert len(cached) + len(dumped) == 10


@pytest.mark.skipif(not has_files, reason="test needs a local filesystem")
def test_http_retry_multiple_rate_limit(cmake, httpserver, unreachable_dsn):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})
    db_dir = tmp_path.joinpath(".sentry-native")
    cache_dir = db_dir.joinpath("cache")

    run(
        tmp_path,
        "sentry_example",
        ["log", "http-retry", "capture-multiple"],
        env=dict(os.environ, SENTRY_DSN=unreachable_dsn),
    )

    # envelopes end up in cache/ (retry) or *.run/ (dumped on shutdown timeout)
    cached = list(cache_dir.glob("*.envelope"))
    dumped = list(db_dir.glob("*.run/*.envelope"))
    assert len(cached) + len(dumped) == 10

    # rate limit response followed by discards for the rest (rate limiter
    # kicks in after the first 429)
    httpserver.expect_request("/api/123456/envelope/").respond_with_data(
        "Rate Limited", status=429, headers={"retry-after": "60"}
    )

    run(
        tmp_path,
        "sentry_example",
        ["log", "http-retry", "no-setup"],
        env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
    )

    # first envelope gets 429, rest are discarded by rate limiter
    cache_files = list(cache_dir.glob("*.envelope"))
    assert len(cache_files) == 0


@pytest.mark.skipif(not has_files, reason="test needs a local filesystem")
def test_http_retry_session_on_network_error(cmake, httpserver, unreachable_dsn):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": "none"})
    cache_dir = tmp_path.joinpath(".sentry-native/cache")

    env = dict(os.environ, SENTRY_DSN=unreachable_dsn)

    run(
        tmp_path,
        "sentry_example",
        ["log", "http-retry", "start-session"],
        env=env,
    )

    assert cache_dir.exists()
    cache_files = list(cache_dir.glob("*.envelope"))
    assert len(cache_files) == 1
    assert "-00-" in str(cache_files[0].name)
    envelope_uuid = cache_files[0].stem[-36:]

    # second and third attempts still fail — envelope gets renamed each time
    run(
        tmp_path,
        "sentry_example",
        ["log", "http-retry", "no-setup"],
        env=env,
    )

    cache_files = list(cache_dir.glob("*.envelope"))
    assert len(cache_files) == 1
    assert "-01-" in str(cache_files[0].name)
    assert cache_files[0].stem[-36:] == envelope_uuid

    run(
        tmp_path,
        "sentry_example",
        ["log", "http-retry", "no-setup"],
        env=env,
    )

    cache_files = list(cache_dir.glob("*.envelope"))
    assert len(cache_files) == 1
    assert "-02-" in str(cache_files[0].name)
    assert cache_files[0].stem[-36:] == envelope_uuid

    # succeed on fourth attempt
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
    assert_session(envelope, {"init": True, "status": "exited", "errors": 0})

    cache_files = list(cache_dir.glob("*.envelope"))
    assert len(cache_files) == 0


@pytest.mark.parametrize(
    "backend",
    [
        "inproc",
        pytest.param(
            "breakpad",
            marks=pytest.mark.skipif(
                not has_breakpad or is_qemu, reason="test needs breakpad backend"
            ),
        ),
    ],
)
@pytest.mark.skipif(is_qemu, reason="unreliable under qemu-user")
def test_restart_on_crash(cmake, httpserver, backend):
    tmp_path = cmake(["sentry_example"], {"SENTRY_BACKEND": backend})

    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")
    httpserver.expect_oneshot_request(
        "/api/123456/envelope/",
        headers={"x-sentry-auth": auth_header},
    ).respond_with_data("OK")

    with httpserver.wait(timeout=10) as waiting:
        # The restarted child inherits stdio, so PIPE waits for it without a sleep.
        run(
            tmp_path,
            "sentry_example",
            ["crash", "restart-on-crash"],
            expect_failure=True,
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        run(
            tmp_path,
            "sentry_example",
            ["log", "no-setup"],
            env=dict(os.environ, SENTRY_DSN=make_dsn(httpserver)),
        )

    assert waiting.result
    assert len(httpserver.log) == 2
    for req in httpserver.log:
        envelope = Envelope.deserialize(req[0].get_data())
        if backend == "inproc":
            assert_inproc_crash(envelope)
        elif backend == "breakpad":
            assert_breakpad_crash(envelope)
        else:
            assert False
