import email
import gzip
import json
import platform
import re
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, UTC
from pathlib import Path

import tests

import msgpack

from . import REPLAY_ID, SENTRY_VERSION
from .conditions import is_android, is_asan, is_tsan

VERSION_RE = re.compile(r"(\d+\.\d+\.\d+)[-.]?(.*)")

INSTALLATION_ID_RE = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"
)


def matches(actual, expected):
    return {k: v for (k, v) in actual.items() if k in expected.keys()} == expected


def assert_matches(actual, expected):
    """Assert two objects for equality, ignoring extra keys in ``actual``."""
    assert {k: v for (k, v) in actual.items() if k in expected.keys()} == expected


def assert_session(
    envelope,
    extra_assertion=None,
    release="test-example-release",
    environment="development",
):
    session = None
    for item in envelope:
        if item.headers.get("type") == "session" and item.payload.json is not None:
            session = item.payload.json

    assert session is not None
    assert INSTALLATION_ID_RE.match(session["did"]), session["did"]
    assert session["attrs"] == {
        "release": release,
        "environment": environment,
    }
    if extra_assertion:
        assert_matches(session, extra_assertion)


def assert_user_feedback(envelope):
    user_feedback = None
    for item in envelope:
        if item.headers.get("type") == "feedback" and item.payload.json is not None:
            user_feedback = item.payload.json["contexts"]["feedback"]

    assert user_feedback is not None
    assert user_feedback["name"] == "some-name"
    assert user_feedback["contact_email"] == "some-email"
    assert user_feedback["message"] == "some-message"


def assert_user_report(envelope):
    user_report = None
    for item in envelope:
        if item.headers.get("type") == "user_report" and item.payload.json is not None:
            user_report = item.payload.json

    assert user_report is not None
    assert user_report["name"] == "some-name"
    assert user_report["email"] == "some-email"
    assert user_report["comments"] == "some-comment"


def assert_meta(
    envelope,
    release="test-example-release",
    integration=None,
    transaction="test-transaction",
    transaction_data=None,
    sdk_override=None,
):
    assert envelope.headers["event_id"]
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
    assert event["event_id"]

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
        "transaction": transaction,
        "tags": {"expected-tag": "some value"},
        "extra": extra,
    }
    expected_sdk = {
        "name": "sentry.native",
        "version": SENTRY_VERSION,
        "packages": [
            {"name": "github:getsentry/sentry-native", "version": SENTRY_VERSION},
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
            expected_os_context = {"name": "Linux", "version": version}
            if build:
                expected_os_context["build"] = build

            assert_matches(event["contexts"]["os"], expected_os_context)
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
    assert event["user"]["username"] == "some_name"
    assert INSTALLATION_ID_RE.match(event["user"]["id"])
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


def assert_debug_meta_images_do_not_overlap(event):
    """Validate ``event["debug_meta"]["images"]`` shape.

    The half-open ranges ``[image_addr, image_addr + image_size)`` must not
    overlap -- the symbolicator relies on this to attribute frames to images.
    """
    images = event["debug_meta"]["images"]
    assert len(images) > 0, "debug_meta should contain at least one image"

    ranges = []
    for image in images:
        name = image.get("code_file") or image.get("debug_file") or "<unknown>"
        addr = int(image["image_addr"], 16)
        ranges.append((addr, addr + image["image_size"], name))

    ranges.sort()
    for (a_start, a_end, a_name), (b_start, b_end, b_name) in zip(ranges, ranges[1:]):
        assert a_end <= b_start, (
            f"image ranges overlap: {a_name} [{a_start:#x}, {a_end:#x}) "
            f"vs {b_name} [{b_start:#x}, {b_end:#x})"
        )


def is_valid_hex(s):
    if not s.lower().startswith("0x"):
        return False
    try:
        int(s, 0)
        return True
    except ValueError:
        return False


def assert_stacktrace(
    envelope, inside_exception=False, check_size=True, check_package=False
):
    event = envelope.get_event()

    parent = event["exception"] if inside_exception else event["threads"]
    frames = parent["values"][0]["stacktrace"]["frames"]
    assert isinstance(frames, list)

    if check_size:
        assert len(frames) > 0
        assert all(is_valid_hex(frame["instruction_addr"]) for frame in frames)
        assert any(
            frame.get("function") is not None and frame.get("package") is not None
            for frame in frames
        )

    if check_package:
        for frame in frames:
            frame_package = frame.get("package")
            if frame_package is not None:
                frame_package_path = Path(frame_package)
                # only assert on absolute paths, since letting pathlib resolve relative paths is cheating
                if frame_package_path.is_absolute():
                    assert (
                        frame_package_path.is_file()
                    ), f"package is not a valid file path: '{frame_package}'"


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


def assert_logs(envelope, expected_item_count=1, expected_trace_id=None):
    logs = None
    for item in envelope:
        if item.headers.get("type") == "client_report":
            continue
        assert item.headers.get("type") == "log"
        # >= because of random #lost logs in test_logs_threaded
        assert item.headers.get("item_count") >= expected_item_count
        assert (
            item.headers.get("content_type") == "application/vnd.sentry.items.log+json"
        )
        logs = item.payload.json

    assert isinstance(logs, dict)
    assert "items" in logs
    # >= because of random #lost logs in test_logs_threaded
    assert len(logs["items"]) >= expected_item_count
    for i in range(expected_item_count):
        log_item = logs["items"][i]
        assert "body" in log_item
        assert "level" in log_item
        assert "timestamp" in log_item  # TODO do we need to validate the timestamp?
        assert "trace_id" in log_item
        assert "attributes" in log_item
        assert "os.name" in log_item["attributes"]
        assert "os.version" in log_item["attributes"]
        assert "sentry.environment" in log_item["attributes"]
        assert "sentry.release" in log_item["attributes"]
        assert "sentry.sdk.name" in log_item["attributes"]
        assert "sentry.sdk.version" in log_item["attributes"]
        if expected_trace_id:
            assert log_item["trace_id"] == expected_trace_id


def assert_metrics(envelope, expected_item_count=1, expected_trace_id=None):
    metrics = None
    for item in envelope:
        if item.headers.get("type") == "client_report":
            continue
        assert item.headers.get("type") == "trace_metric"
        assert item.headers.get("item_count") >= expected_item_count
        assert (
            item.headers.get("content_type")
            == "application/vnd.sentry.items.trace-metric+json"
        )
        metrics = item.payload.json

    assert isinstance(metrics, dict)
    assert "items" in metrics
    assert len(metrics["items"]) >= expected_item_count
    for i in range(expected_item_count):
        metric_item = metrics["items"][i]
        assert "name" in metric_item
        assert "type" in metric_item
        assert metric_item["type"] in ["counter", "gauge", "distribution"]
        assert "value" in metric_item
        assert "timestamp" in metric_item
        assert "trace_id" in metric_item
        assert "attributes" in metric_item
        attrs = metric_item["attributes"]
        assert "sentry.environment" in attrs
        assert "sentry.release" in attrs
        assert "sentry.sdk.name" in attrs
        assert "sentry.sdk.version" in attrs
        if expected_trace_id:
            assert metric_item["trace_id"] == expected_trace_id


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
    dt = datetime.fromisoformat(ts)
    # 1s tolerance for `date +%s` truncation in device clock offset measurement
    assert dt <= tests.now() + timedelta(seconds=1), "timestamp is in the future"
    assert dt >= tests.test_start, "timestamp is in the past"


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


def assert_native_crash(envelope, exception_code=None):
    assert envelope.headers["event_id"]
    event = envelope.get_event()
    assert event is not None
    assert event["event_id"]
    assert_matches(event, {"level": "fatal"})

    exc = event["exception"]["values"][0]
    assert exc["mechanism"]["type"] == "signalhandler"
    assert exc["mechanism"]["handled"] is False
    if exception_code is not None:
        assert exc["mechanism"]["meta"]["signal"]["number"] == exception_code
    assert "stacktrace" in exc
    assert len(exc["stacktrace"]["frames"]) > 0


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


def assert_before_breadcrumb(envelope):
    event = envelope.get_event()
    breadcrumbs = event.get("breadcrumbs", [])
    assert len(breadcrumbs) > 0
    assert all(b.get("category") == "before_breadcrumb" for b in breadcrumbs)


def assert_no_breadcrumbs(envelope):
    event = envelope.get_event()
    breadcrumbs = event.get("breadcrumbs")
    assert not breadcrumbs


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
        assert attachments.bytes_bin == b"\xc0\xff\xee"
    else:
        assert attachments.cmake_cache == -1
        assert attachments.bytes_bin == None
    if expect_view_hierarchy:
        assert_attachment_content_view_hierarchy(attachments.view_hierarchy)
    assert any(
        b'name="upload_file_minidump"' in part.as_bytes()
        and b"\n\nMDMP" in part.as_bytes()
        for part in msg.walk()
    )
    return attachments


def assert_gzip_file_header(output):
    assert output[:3] == b"\x1f\x8b\x08"


def assert_gzip_content_encoding(req):
    assert req.content_encoding == "gzip"


def assert_client_report(envelope, expected_discards=None):
    """
    Assert that the envelope contains a client_report item.

    Args:
        envelope: The envelope to check
        expected_discards: Optional list of dicts with expected discarded_events entries.
            Each dict should have 'reason', 'category', and optionally 'quantity' keys.
            If quantity is not specified, it just checks that count > 0.
    """
    client_report = None
    for item in envelope:
        if (
            item.headers.get("type") == "client_report"
            and item.payload.json is not None
        ):
            client_report = item.payload.json
            break

    assert client_report is not None, "No client_report item found in envelope"

    # Check timestamp exists and is valid
    assert "timestamp" in client_report
    assert_timestamp(client_report["timestamp"])

    # Check discarded_events array exists
    assert "discarded_events" in client_report
    discarded_events = client_report["discarded_events"]
    assert isinstance(discarded_events, list)
    assert len(discarded_events) > 0

    # Validate each discarded event entry
    for entry in discarded_events:
        assert "reason" in entry
        assert "category" in entry
        assert "quantity" in entry
        assert entry["quantity"] > 0

    # Check expected discards if provided
    if expected_discards:
        for expected in expected_discards:
            found = False
            for entry in discarded_events:
                if (
                    entry["reason"] == expected["reason"]
                    and entry["category"] == expected["category"]
                ):
                    if "quantity" in expected:
                        assert entry["quantity"] == expected["quantity"], (
                            f"Expected quantity {expected['quantity']} for {expected['reason']}/{expected['category']}, "
                            f"got {entry['quantity']}"
                        )
                    found = True
                    break
            assert found, (
                f"Expected discard entry with reason={expected['reason']}, "
                f"category={expected['category']} not found"
            )


def assert_no_proxy_request(stdout):
    assert "POST" not in stdout


def assert_failed_proxy_auth_request(stdout):
    assert (
        "POST" in stdout
        and "407 Proxy Authentication Required" in stdout
        and "200 OK" not in stdout
    )


def wait_for(condition, timeout=10.0, interval=0.1):
    import time

    deadline = time.time() + timeout
    while time.time() < deadline:
        if condition():
            return True
        time.sleep(interval)
    return False


def wait_for_file(path, timeout=10.0, interval=0.1):
    import glob

    return wait_for(lambda: glob.glob(str(path)), timeout, interval)


def wait_for_daemon(tmp_path, started_at, timeout=None):
    import time

    if timeout is None:
        timeout = 30.0 if is_asan or is_tsan else 10.0

    db_dir = Path(tmp_path) / ".sentry-native"
    # Account for filesystems that truncate mtimes below time.time() precision.
    started_at -= 1.0

    deadline = time.time() + timeout
    while time.time() < deadline:
        for log_path in db_dir.glob("sentry-daemon-*.log"):
            try:
                if log_path.stat().st_mtime < started_at:
                    continue
                log = log_path.read_text(errors="replace")
            except OSError:
                continue

            if "Marking crash state as DONE" in log:
                return True

        time.sleep(0.1)

    return False


def wait_for_stdout(process, predicate, timeout=10):
    """Read a process's stdout in a background thread, polling until
    *predicate(text)* is satisfied or *timeout* expires.

    Returns the full stdout text collected up to that point.
    The caller should terminate/kill the process after this returns
    to close the pipe and let the background thread exit.
    """
    import threading

    lines = []

    def reader():
        try:
            for line in iter(process.stdout.readline, b""):
                lines.append(line)
        except ValueError:
            pass

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()

    def stdout_text():
        return b"".join(lines).decode("utf-8", errors="replace")

    if not wait_for(lambda: predicate(stdout_text()), timeout):
        raise TimeoutError(
            f"Predicate not satisfied within {timeout}s.\n"
            f"Collected output:\n{stdout_text()}"
        )
    return stdout_text()


def assert_replay_envelope(envelope, video, replay_id=REPLAY_ID):
    """Validate a `replay_video` envelope built from the fixture staged by
    `tests.stage_replay` (the sidecar values below match that fixture)."""
    assert envelope.headers["event_id"] == replay_id

    (item,) = envelope.items
    assert item.headers["type"] == "replay_video"
    body = msgpack.unpackb(item.payload.bytes)
    assert set(body) == {"replay_event", "replay_recording", "replay_video"}

    event = json.loads(body["replay_event"])
    assert event["type"] == "replay_event"
    assert event["replay_id"] == replay_id
    assert event["event_id"] == replay_id
    assert event["segment_id"] == 0
    assert event["replay_type"] == "buffer"
    assert event["platform"] == "native"

    # scope fields are copied over from the crash event
    assert event["tags"]["expected-tag"] == "some value"
    assert isinstance(event["trace_ids"], list)

    header, _, rrweb = body["replay_recording"].partition(b"\n")
    assert json.loads(header) == {"segment_id": 0}
    meta_event, video_event = json.loads(rrweb)
    assert meta_event["type"] == 4
    assert meta_event["data"]["width"] == 3864
    assert meta_event["data"]["height"] == 2100
    assert video_event["type"] == 5
    assert video_event["data"]["tag"] == "video"
    payload = video_event["data"]["payload"]
    assert payload["segmentId"] == 0
    assert payload["size"] == len(video)
    assert payload["duration"] == 15077
    assert payload["encoding"] == "h264"
    assert payload["container"] == "mp4"
    assert payload["frameCount"] == 58
    assert payload["frameRate"] == 30

    assert body["replay_video"] == video
