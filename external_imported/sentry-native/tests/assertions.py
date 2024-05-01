import email
import gzip
import platform
import re
import sys
from dataclasses import dataclass
from datetime import datetime

import msgpack

from .conditions import is_android

VERSION_RE = re.compile(r"(\d+\.\d+\.\d+)[-.]?(.*)")


def matches(actual, expected):
    return {k: v for (k, v) in actual.items() if k in expected.keys()} == expected


def assert_matches(actual, expected):
    """Assert two objects for equality, ignoring extra keys in ``actual``."""
    assert {k: v for (k, v) in actual.items() if k in expected.keys()} == expected


def assert_session(envelope, extra_assertion=None):
    session = None
    for item in envelope:
        if item.headers.get("type") == "session" and item.payload.json is not None:
            session = item.payload.json

    assert session is not None
    assert session["did"] == "42"
    assert session["attrs"] == {
        "release": "test-example-release",
        "environment": "development",
    }
    if extra_assertion:
        assert_matches(session, extra_assertion)


def assert_meta(
    envelope,
    release="test-example-release",
    integration=None,
    transaction="test-transaction",
    sdk_override=None,
):
    event = envelope.get_event()

    expected = {
        "platform": "native",
        "environment": "development",
        "release": release,
        "user": {"id": 42, "username": "some_name"},
        "transaction": transaction,
        "tags": {"expected-tag": "some value"},
        "extra": {"extra stuff": "some value", "…unicode key…": "őá…–🤮🚀¿ 한글 테스트"},
    }
    expected_sdk = {
        "name": "sentry.native",
        "version": "0.7.0",
        "packages": [
            {"name": "github:getsentry/sentry-native", "version": "0.7.0"},
        ],
    }
    if is_android:
        expected_sdk["name"] = "sentry.native.android"
    else:
        if sys.platform == "win32":
            assert_matches(
                event["contexts"]["os"],
                {"name": "Windows", "version": platform.version()},
            )
            assert event["contexts"]["os"]["build"] is not None
        elif sys.platform == "linux":
            version = platform.release()
            match = VERSION_RE.match(version)
            version = match.group(1)
            build = match.group(2)

            assert_matches(
                event["contexts"]["os"],
                {"name": "Linux", "version": version, "build": build},
            )
        elif sys.platform == "darwin":
            version = platform.mac_ver()[0].split(".")
            if len(version) < 3:
                version.append("0")
            version = ".".join(version)

            assert_matches(
                event["contexts"]["os"],
                {
                    "name": "macOS",
                    "version": version,
                    "kernel_version": platform.release(),
                },
            )
            assert event["contexts"]["os"]["build"] is not None

    if sdk_override is not None:
        expected_sdk["name"] = sdk_override

    assert_matches(event, expected)
    assert_matches(event["sdk"], expected_sdk)
    assert_matches(
        event["contexts"], {"runtime": {"type": "runtime", "name": "testing-runtime"}}
    )

    if integration is None:
        assert event["sdk"].get("integrations") is None
    else:
        assert event["sdk"]["integrations"] == [integration]
    if event.get("type") == "event":
        assert any(
            "sentry_example" in image["code_file"]
            for image in event["debug_meta"]["images"]
        )


def assert_stacktrace(envelope, inside_exception=False, check_size=True):
    event = envelope.get_event()

    parent = event["exception"] if inside_exception else event["threads"]
    frames = parent["values"][0]["stacktrace"]["frames"]
    assert isinstance(frames, list)

    if check_size:
        assert len(frames) > 0
        assert all(frame["instruction_addr"].startswith("0x") for frame in frames)
        assert any(
            frame.get("function") is not None and frame.get("package") is not None
            for frame in frames
        )


def assert_breadcrumb(envelope):
    event = envelope.get_event()

    expected = {
        "type": "http",
        "message": "debug crumb",
        "category": "example!",
        "level": "debug",
    }
    assert any(matches(b, expected) for b in event["breadcrumbs"])


def assert_attachment(envelope):
    expected = {
        "type": "attachment",
        "filename": "CMakeCache.txt",
    }
    assert any(matches(item.headers, expected) for item in envelope)


def assert_minidump(envelope):
    expected = {
        "type": "attachment",
        "attachment_type": "event.minidump",
    }
    minidump = next(item for item in envelope if matches(item.headers, expected))
    assert minidump.headers["length"] > 4
    assert minidump.payload.bytes.startswith(b"MDMP")


def assert_timestamp(ts, now=datetime.utcnow()):
    assert ts[:11] == now.isoformat()[:11]


def assert_event(envelope):
    event = envelope.get_event()
    expected = {
        "level": "info",
        "logger": "my-logger",
        "message": {"formatted": "Hello World!"},
    }
    assert_matches(event, expected)
    assert_timestamp(event["timestamp"])


def assert_breakpad_crash(envelope):
    event = envelope.get_event()
    expected = {
        "level": "fatal",
    }
    assert_matches(event, expected)


def assert_exception(envelope):
    event = envelope.get_event()
    exception = {
        "type": "ParseIntError",
        "value": "invalid digit found in string",
    }
    assert_matches(event["exception"]["values"][0], exception)
    assert_timestamp(event["timestamp"])


def assert_inproc_crash(envelope):
    event = envelope.get_event()
    assert_matches(event, {"level": "fatal"})
    # depending on the unwinder, we currently don’t get any stack frames from
    # a `ucontext`
    assert_stacktrace(envelope, inside_exception=True, check_size=False)


def assert_crash_timestamp(has_files, tmp_path):
    # The crash file should survive a `sentry_init` and should still be there
    # even after restarts.
    if has_files:
        with open("{}/.sentry-native/last_crash".format(tmp_path)) as f:
            crash_timestamp = f.read()
        assert_timestamp(crash_timestamp)


def assert_before_send(envelope):
    event = envelope.get_event()
    assert_matches(event, {"adapted_by": "before_send"})


def assert_no_before_send(envelope):
    event = envelope.get_event()
    assert ("adapted_by", "before_send") not in event.items()


@dataclass(frozen=True)
class CrashpadAttachments:
    event: dict
    breadcrumb1: list
    breadcrumb2: list


def _unpack_breadcrumbs(payload):
    unpacker = msgpack.Unpacker()
    unpacker.feed(payload)
    return [unpacked for unpacked in unpacker]


def _load_crashpad_attachments(msg):
    event = {}
    breadcrumb1 = []
    breadcrumb2 = []
    for part in msg.walk():
        match part.get_filename():
            case "__sentry-event":
                event = msgpack.unpackb(part.get_payload(decode=True))
            case "__sentry-breadcrumb1":
                breadcrumb1 = _unpack_breadcrumbs(part.get_payload(decode=True))
            case "__sentry-breadcrumb2":
                breadcrumb2 = _unpack_breadcrumbs(part.get_payload(decode=True))

    return CrashpadAttachments(event, breadcrumb1, breadcrumb2)


def is_valid_timestamp(timestamp):
    try:
        datetime.fromisoformat(timestamp)
        return True
    except ValueError:
        return False


def _validate_breadcrumb_seq(seq, breadcrumb_func):
    for i in seq:
        breadcrumb = breadcrumb_func(i)
        assert breadcrumb["message"] == str(i)
        assert is_valid_timestamp(breadcrumb["timestamp"])


def assert_crashpad_upload(req):
    multipart = gzip.decompress(req.get_data())
    msg = email.message_from_bytes(bytes(str(req.headers), encoding="utf8") + multipart)
    attachments = _load_crashpad_attachments(msg)

    if len(attachments.breadcrumb1) > 3:
        _validate_breadcrumb_seq(range(97), lambda i: attachments.breadcrumb1[3 + i])
        _validate_breadcrumb_seq(
            range(97, 101), lambda i: attachments.breadcrumb2[i - 97]
        )

    assert attachments.event["level"] == "fatal"

    assert any(
        b'name="upload_file_minidump"' in part.as_bytes()
        and b"\n\nMDMP" in part.as_bytes()
        for part in msg.walk()
    )
