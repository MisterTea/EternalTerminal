import datetime
import email
import gzip
import sys
import platform
import re
from .conditions import is_android


VERSION_RE = re.compile(r"(\d+\.\d+\.\d+)(?:[-\.]?)(.*)")


def matches(actual, expected):
    return {k: v for (k, v) in actual.items() if k in expected.keys()} == expected


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
        assert matches(session, extra_assertion)


def assert_meta(envelope, release="test-example-release", integration=None):
    event = envelope.get_event()

    expected = {
        "platform": "native",
        "environment": "development",
        "release": release,
        "user": {"id": 42, "username": "some_name"},
        "transaction": "test-transaction",
        "tags": {"expected-tag": "some value"},
        "extra": {"extra stuff": "some value", "…unicode key…": "őá…–🤮🚀¿ 한글 테스트"},
    }
    expected_sdk = {
        "name": "sentry.native",
        "version": "0.4.8",
        "packages": [{"name": "github:getsentry/sentry-native", "version": "0.4.8"},],
    }
    if not is_android:
        if sys.platform == "win32":
            assert matches(
                event["contexts"]["os"],
                {"name": "Windows", "version": platform.version()},
            )
            assert event["contexts"]["os"]["build"] is not None
        elif sys.platform == "linux":
            version = platform.release()
            match = VERSION_RE.match(version)
            version = match.group(1)
            build = match.group(2)

            assert matches(
                event["contexts"]["os"],
                {"name": "Linux", "version": version, "build": build},
            )
        elif sys.platform == "darwin":
            version = platform.mac_ver()[0].split(".")
            if len(version) < 3:
                version.append("0")
            version = ".".join(version)

            assert matches(
                event["contexts"]["os"],
                {
                    "name": "macOS",
                    "version": version,
                    "kernel_version": platform.release(),
                },
            )
            assert event["contexts"]["os"]["build"] is not None

    assert matches(event, expected)
    assert matches(event["sdk"], expected_sdk)
    assert matches(
        event["contexts"], {"runtime": {"type": "runtime", "name": "testing-runtime"}}
    )

    if integration is None:
        assert event["sdk"].get("integrations") is None
    else:
        assert event["sdk"]["integrations"] == [integration]
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


def assert_timestamp(ts, now=datetime.datetime.utcnow()):
    assert ts[:11] == now.isoformat()[:11]


def assert_event(envelope):
    event = envelope.get_event()
    expected = {
        "level": "info",
        "logger": "my-logger",
        "message": {"formatted": "Hello World!"},
    }
    assert matches(event, expected)
    assert_timestamp(event["timestamp"])


def assert_exception(envelope):
    event = envelope.get_event()
    exception = {
        "type": "ParseIntError",
        "value": "invalid digit found in string",
    }
    expected = {"exception": {"values": [exception]}}
    assert matches(event, expected)
    assert_timestamp(event["timestamp"])


def assert_crash(envelope):
    event = envelope.get_event()
    assert matches(event, {"level": "fatal"})
    # depending on the unwinder, we currently don’t get any stack frames from
    # a `ucontext`
    assert_stacktrace(envelope, inside_exception=True, check_size=False)


def assert_crashpad_upload(req):
    multipart = gzip.decompress(req.get_data())
    msg = email.message_from_bytes(bytes(str(req.headers), encoding="utf8") + multipart)
    files = [part.get_filename() for part in msg.walk()]

    # TODO:
    # Actually assert that we get a correct event/breadcrumbs payload
    assert "__sentry-breadcrumb1" in files
    assert "__sentry-breadcrumb2" in files
    assert "__sentry-event" in files

    assert any(
        b'name="upload_file_minidump"' in part.as_bytes()
        and b"\n\nMDMP" in part.as_bytes()
        for part in msg.walk()
    )
