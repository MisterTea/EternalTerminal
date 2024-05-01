import os
import subprocess
import sys
import time

import pytest

from . import check_output, run, Envelope
from .assertions import (
    assert_attachment,
    assert_meta,
    assert_breadcrumb,
    assert_stacktrace,
    assert_event,
    assert_inproc_crash,
    assert_minidump,
    assert_before_send,
    assert_no_before_send,
    assert_crash_timestamp,
    assert_breakpad_crash,
)
from .conditions import has_breakpad, has_files


def test_capture_stdout(cmake):
    tmp_path = cmake(
        ["sentry_example"],
        {
            "SENTRY_BACKEND": "none",
            "SENTRY_TRANSPORT": "none",
        },
    )

    output = check_output(
        tmp_path,
        "sentry_example",
        ["stdout", "attachment", "capture-event", "add-stacktrace"],
    )
    envelope = Envelope.deserialize(output)

    assert_meta(envelope)
    assert_breadcrumb(envelope)
    assert_attachment(envelope)
    assert_stacktrace(envelope)

    assert_event(envelope)


def test_dynamic_sdk_name_override(cmake):
    tmp_path = cmake(
        ["sentry_example"],
        {
            "SENTRY_BACKEND": "none",
            "SENTRY_TRANSPORT": "none",
        },
    )

    output = check_output(
        tmp_path,
        "sentry_example",
        ["stdout", "override-sdk-name", "capture-event"],
    )
    envelope = Envelope.deserialize(output)

    assert_meta(envelope, sdk_override="sentry.native.android.flutter")
    assert_event(envelope)


def test_sdk_name_override(cmake):
    sdk_name = "cUsToM.SDK"
    tmp_path = cmake(
        ["sentry_example"],
        {
            "SENTRY_BACKEND": "none",
            "SENTRY_TRANSPORT": "none",
            "SENTRY_SDK_NAME": sdk_name,
        },
    )

    output = check_output(
        tmp_path,
        "sentry_example",
        ["stdout", "capture-event"],
    )
    envelope = Envelope.deserialize(output)

    assert_meta(envelope, sdk_override=sdk_name)
    assert_event(envelope)


@pytest.mark.skipif(not has_files, reason="test needs a local filesystem")
def test_multi_process(cmake):
    # NOTE: It would have been nice to do *everything* in a unicode-named
    # directory, but apparently cmake does not like that either.
    tmp_path = cmake(
        ["sentry_example"],
        {"SENTRY_BACKEND": "none", "SENTRY_TRANSPORT": "none"},
    )

    cwd = tmp_path.joinpath("unicode ❤️ Юля")
    cwd.mkdir()
    exe = "sentry_example"
    cmd = (
        "../{}".format(exe)
        if sys.platform != "win32"
        else "{}\\{}.exe".format(tmp_path, exe)
    )

    child1 = subprocess.Popen([cmd, "sleep"], cwd=cwd)
    child2 = subprocess.Popen([cmd, "sleep"], cwd=cwd)
    time.sleep(0.5)

    # while the processes are running, we expect two runs
    runs = [
        db_run
        for db_run in os.listdir(os.path.join(cwd, ".sentry-native"))
        if db_run.endswith(".run")
    ]
    assert len(runs) == 2

    # kill the children
    child1.terminate()
    child2.terminate()
    child1.wait()
    child2.wait()

    # and start another process that cleans up the old runs
    subprocess.run([cmd], cwd=cwd)

    runs = [
        db_run
        for db_run in os.listdir(os.path.join(cwd, ".sentry-native"))
        if db_run.endswith(".run") or db_run.endswith(".lock")
    ]
    assert len(runs) == 0


def run_crash_stdout_for(backend, cmake, example_args):
    tmp_path = cmake(
        ["sentry_example"],
        {"SENTRY_BACKEND": backend, "SENTRY_TRANSPORT": "none"},
    )

    child = run(tmp_path, "sentry_example", ["attachment", "crash"] + example_args)
    assert child.returncode  # well, it's a crash after all

    return tmp_path, check_output(tmp_path, "sentry_example", ["stdout", "no-setup"])


def test_inproc_crash_stdout(cmake):
    tmp_path, output = run_crash_stdout_for("inproc", cmake, [])

    envelope = Envelope.deserialize(output)

    assert_crash_timestamp(has_files, tmp_path)
    assert_meta(envelope, integration="inproc")
    assert_breadcrumb(envelope)
    assert_attachment(envelope)
    assert_inproc_crash(envelope)


def test_inproc_crash_stdout_before_send(cmake):
    tmp_path, output = run_crash_stdout_for("inproc", cmake, ["before-send"])

    envelope = Envelope.deserialize(output)

    assert_crash_timestamp(has_files, tmp_path)
    assert_meta(envelope, integration="inproc")
    assert_breadcrumb(envelope)
    assert_attachment(envelope)
    assert_inproc_crash(envelope)
    assert_before_send(envelope)


def test_inproc_crash_stdout_discarding_on_crash(cmake):
    tmp_path, output = run_crash_stdout_for("inproc", cmake, ["discarding-on-crash"])

    # since the on_crash() handler discards further processing we expect an empty response
    assert len(output) == 0

    assert_crash_timestamp(has_files, tmp_path)


def test_inproc_crash_stdout_before_send_and_on_crash(cmake):
    tmp_path, output = run_crash_stdout_for(
        "inproc", cmake, ["before-send", "on-crash"]
    )

    # the on_crash() hook retains the event
    envelope = Envelope.deserialize(output)
    # but we expect no event modification from before_send() since setting on_crash() disables before_send()
    assert_no_before_send(envelope)

    assert_crash_timestamp(has_files, tmp_path)
    assert_meta(envelope, integration="inproc")
    assert_breadcrumb(envelope)
    assert_attachment(envelope)
    assert_inproc_crash(envelope)


@pytest.mark.skipif(not has_breakpad, reason="test needs breakpad backend")
def test_breakpad_crash_stdout(cmake):
    tmp_path, output = run_crash_stdout_for("breakpad", cmake, [])

    envelope = Envelope.deserialize(output)

    assert_crash_timestamp(has_files, tmp_path)
    assert_meta(envelope, integration="breakpad")
    assert_breadcrumb(envelope)
    assert_attachment(envelope)
    assert_minidump(envelope)
    assert_breakpad_crash(envelope)


@pytest.mark.skipif(not has_breakpad, reason="test needs breakpad backend")
def test_breakpad_crash_stdout_before_send(cmake):
    tmp_path, output = run_crash_stdout_for("breakpad", cmake, ["before-send"])

    envelope = Envelope.deserialize(output)

    assert_crash_timestamp(has_files, tmp_path)
    assert_meta(envelope, integration="breakpad")
    assert_breadcrumb(envelope)
    assert_attachment(envelope)
    assert_minidump(envelope)
    assert_before_send(envelope)
    assert_breakpad_crash(envelope)


@pytest.mark.skipif(not has_breakpad, reason="test needs breakpad backend")
def test_breakpad_crash_stdout_discarding_on_crash(cmake):
    tmp_path, output = run_crash_stdout_for("breakpad", cmake, ["discarding-on-crash"])

    # since the on_crash() handler discards further processing we expect an empty response
    assert len(output) == 0

    assert_crash_timestamp(has_files, tmp_path)


@pytest.mark.skipif(not has_breakpad, reason="test needs breakpad backend")
def test_breakpad_crash_stdout_before_send_and_on_crash(cmake):
    tmp_path, output = run_crash_stdout_for(
        "breakpad", cmake, ["before-send", "on-crash"]
    )

    # the on_crash() hook retains the event
    envelope = Envelope.deserialize(output)
    # but we expect no event modification from before_send() since setting on_crash() disables before_send()
    assert_no_before_send(envelope)

    assert_crash_timestamp(has_files, tmp_path)
    assert_meta(envelope, integration="breakpad")
    assert_breadcrumb(envelope)
    assert_attachment(envelope)
    assert_breakpad_crash(envelope)
