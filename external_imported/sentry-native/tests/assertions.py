import email
import gzip
import json
import platform
import re
import sys
from dataclasses import dataclass
from datetime import datetime, UTC

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


def assert_user_feedback(envelope):
    user_feedback = None
    for item in envelope:
        if item.headers.get("type") == "user_report" and item.payload.json is not None:
            user_feedback = item.payload.json

    assert user_feedback is not None
    assert user_feedback["name"] == "some-name"
    assert user_feedback["email"] == "some-email"
    assert user_feedback["comments"] == "some-comment"


def assert_meta(
    envelope,
    release="test-example-release",
    integration=None,
    transaction="test-transaction",
    transaction_data=None,
    sdk_override=None,
):
    event = envelope.get_event()
    assert_event_meta(
        event, release, integration, transaction, transaction_data, sdk_override
    )


def assert_event_meta(
    event,
    release="test-example-release",
    integration=None,
    transaction="test-transaction",
    transaction_data=None,
    sdk_override=None,
):
    extra = {
        "extra stuff": "some value",
        "…unicode key…": "őá…–🤮🚀¿ 한글 테스트",
    }
    if transaction_data:
        extra.update(transaction_data)

    expected = {
        "platform": "native",
        "environment": "development",
        "release": release,
        "user": {"id": "42", "username": "some_name"},
        "transaction": transaction,
        "tags": {"expected-tag": "some value"},
        "extra": extra,
    }
    expected_sdk = {
        "name": "sentry.native",
        "version": "0.9.1",
        "packages": [
            {"name": "github:getsentry/sentry-native", "version": "0.9.1"},
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
            assert "distribution_name" in event["contexts"]["os"]
            assert "distribution_version" in event["contexts"]["os"]
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


def assert_breadcrumb_inner(breadcrumbs, message="debug crumb"):
    expected = {
        "type": "http",
        "message": message,
        "category": "example!",
        "level": "debug",
        "data": {
            "url": "https://example.com/api/1.0/users",
            "method": "GET",
            "status_code": 200,
            "reason": "OK",
        },
    }
    assert any(matches(b, expected) for b in breadcrumbs)


def assert_breadcrumb(envelope, message="debug crumb"):
    event = envelope.get_event()
    assert_breadcrumb_inner(event["breadcrumbs"], message)


def assert_attachment(envelope):
    expected = {
        "type": "attachment",
        "filename": "CMakeCache.txt",
    }
    assert any(
        matches(item.headers, expected)
        and b"This is the CMakeCache file." in item.payload.bytes
        for item in envelope
    )

    expected = {
        "type": "attachment",
        "filename": "bytes.bin",
        "content_type": "application/octet-stream",
    }
    assert any(
        matches(item.headers, expected) and item.payload.bytes == b"\xc0\xff\xee"
        for item in envelope
    )


def assert_attachment_view_hierarchy(envelope):
    expected = {
        "type": "attachment",
        "filename": "view-hierarchy.json",
        "attachment_type": "event.view_hierarchy",
        "content_type": "application/json",
    }
    assert any(matches(item.headers, expected) for item in envelope)


def assert_attachment_content_view_hierarchy(attachment):
    expected = {
        "rendering_system": "android_view_system",
        "windows": [
            {
                "alpha": 1.0,
                "height": 1280.0,
                "type": "com.android.internal.policy.DecorView",
                "visibility": "visible",
                "width": 768.0,
                "x": 0.0,
                "y": 0.0,
            }
        ],
    }
    assert matches(attachment, expected)


def assert_minidump(envelope):
    expected = {
        "type": "attachment",
        "attachment_type": "event.minidump",
    }
    minidump = next(item for item in envelope if matches(item.headers, expected))
    assert minidump.headers["length"] > 4
    assert minidump.payload.bytes.startswith(b"MDMP")


def assert_timestamp(ts):
    elapsed_time = datetime.now(UTC) - datetime.fromisoformat(ts)
    assert elapsed_time.total_seconds() < 10


def assert_event(envelope, message="Hello World!", expected_trace_id=""):
    event = envelope.get_event()
    expected = {
        "level": "info",
        "logger": "my-logger",
        "message": {"formatted": message},
    }
    assert_matches(event, expected)
    assert_timestamp(event["timestamp"])
    assert_trace_id(event, expected_trace_id)


# if expected_trace is "" we just expect any value to exist
def assert_trace_id(event, expected_trace_id):
    if expected_trace_id == "":
        assert len(event["contexts"]["trace"]["trace_id"]) == 32
    else:
        assert event["contexts"]["trace"]["trace_id"] == expected_trace_id


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
    view_hierarchy: dict
    cmake_cache: int
    bytes_bin: bytes = None


def _unpack_breadcrumbs(payload):
    unpacker = msgpack.Unpacker()
    unpacker.feed(payload)
    return [unpacked for unpacked in unpacker]


def _load_crashpad_attachments(msg):
    event = {}
    breadcrumb1 = []
    breadcrumb2 = []
    view_hierarchy = {}
    cmake_cache = -1
    bytes_bin = None
    for part in msg.walk():
        if part.get_filename() is not None:
            assert part.get("Content-Type") is None

        match part.get_filename():
            case "__sentry-event":
                event = msgpack.unpackb(part.get_payload(decode=True))
            case "__sentry-breadcrumb1":
                breadcrumb1 = _unpack_breadcrumbs(part.get_payload(decode=True))
            case "__sentry-breadcrumb2":
                breadcrumb2 = _unpack_breadcrumbs(part.get_payload(decode=True))
            case "view-hierarchy.json":
                view_hierarchy = json.loads(part.get_payload(decode=True))
            case "CMakeCache.txt":
                cmake_cache = len(part.get_payload(decode=True))
            case "bytes.bin":
                bytes_bin = part.get_payload(decode=True)

    return CrashpadAttachments(
        event, breadcrumb1, breadcrumb2, view_hierarchy, cmake_cache, bytes_bin
    )


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


def assert_overflowing_breadcrumb(attachments):
    if len(attachments.breadcrumb1) > 3:
        _validate_breadcrumb_seq(range(97), lambda i: attachments.breadcrumb1[3 + i])
        _validate_breadcrumb_seq(
            range(97, 101), lambda i: attachments.breadcrumb2[i - 97]
        )
    else:
        assert_breadcrumb_inner(attachments.breadcrumb1)


def assert_crashpad_upload(req, expect_attachment=False, expect_view_hierarchy=False):
    multipart = gzip.decompress(req.get_data())
    msg = email.message_from_bytes(bytes(str(req.headers), encoding="utf8") + multipart)
    attachments = _load_crashpad_attachments(msg)

    assert_overflowing_breadcrumb(attachments)
    assert_event_meta(attachments.event, integration="crashpad")
    if expect_attachment:
        assert attachments.cmake_cache > 0
    else:
        assert attachments.cmake_cache == -1
    if expect_attachment and (sys.platform == "win32" or sys.platform == "linux"):
        assert attachments.bytes_bin == b"\xc0\xff\xee"
    else:
        assert attachments.bytes_bin == None
    if expect_view_hierarchy:
        assert_attachment_content_view_hierarchy(attachments.view_hierarchy)
    assert any(
        b'name="upload_file_minidump"' in part.as_bytes()
        and b"\n\nMDMP" in part.as_bytes()
        for part in msg.walk()
    )


def assert_gzip_file_header(output):
    assert output[:3] == b"\x1f\x8b\x08"


def assert_gzip_content_encoding(req):
    assert req.content_encoding == "gzip"


def assert_no_proxy_request(stdout):
    assert "POST" not in stdout


def assert_failed_proxy_auth_request(stdout):
    assert (
        "POST" in stdout
        and "407 Proxy Authentication Required" in stdout
        and "200 OK" not in stdout
    )
