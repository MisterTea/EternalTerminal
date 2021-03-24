import subprocess
import os
import io
import json
import sys
import urllib
import pytest

sourcedir = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))


# https://docs.pytest.org/en/latest/assert.html#assert-details
pytest.register_assert_rewrite("tests.assertions")


def make_dsn(httpserver, auth="uiaeosnrtdy", id=123456):
    url = urllib.parse.urlsplit(httpserver.url_for("/{}".format(id)))
    # We explicitly use `127.0.0.1` here, because on Windows, `localhost` will
    # first try `::1` (the ipv6 loopback), retry a couple times and give up
    # after a timeout of 2 seconds, falling back to the ipv4 loopback instead.
    host = url.netloc.replace("localhost", "127.0.0.1")
    return urllib.parse.urlunsplit(
        (url.scheme, "{}@{}".format(auth, host), url.path, url.query, url.fragment,)
    )


def run(cwd, exe, args, env=dict(os.environ), **kwargs):
    __tracebackhide__ = True
    if os.environ.get("ANDROID_API"):
        # older android emulators do not correctly pass down the returncode
        # so we basically echo the return code, and parse it manually
        is_pipe = kwargs.get("stdout") == subprocess.PIPE
        kwargs["stdout"] = subprocess.PIPE
        child = subprocess.run(
            [
                "{}/platform-tools/adb".format(os.environ["ANDROID_HOME"]),
                "shell",
                # Android by default only searches for libraries in system
                # directories and the app directory, and only supports RUNPATH
                # since API-24.
                # Since we are no "app" in that sense, we can use
                # `LD_LIBRARY_PATH` to force the android dynamic loader to
                # load `libsentry.so` from the correct library.
                # See https://android.googlesource.com/platform/bionic/+/master/android-changes-for-ndk-developers.md#dt_runpath-support-available-in-api-level-24
                "cd /data/local/tmp && LD_LIBRARY_PATH=. ./{} {}; echo -n ret:$?".format(
                    exe, " ".join(args)
                ),
            ],
            **kwargs,
        )
        stdout = child.stdout
        child.returncode = int(stdout[stdout.rfind(b"ret:") :][4:])
        child.stdout = stdout[: stdout.rfind(b"ret:")]
        if not is_pipe:
            sys.stdout.buffer.write(child.stdout)
        if kwargs.get("check") and child.returncode:
            raise subprocess.CalledProcessError(
                child.returncode, child.args, output=child.stdout, stderr=child.stderr
            )
        return child

    cmd = [
        "./{}".format(exe) if sys.platform != "win32" else "{}\\{}.exe".format(cwd, exe)
    ]
    if "asan" in os.environ.get("RUN_ANALYZER", ""):
        env["ASAN_OPTIONS"] = "detect_leaks=1"
        env["LSAN_OPTIONS"] = "suppressions={}".format(
            os.path.join(sourcedir, "tests", "leaks.txt")
        )
    if "llvm-cov" in os.environ.get("RUN_ANALYZER", ""):
        # continuous mode is only supported on mac right now
        continuous = "%c" if sys.platform == "darwin" else ""
        env["LLVM_PROFILE_FILE"] = f"coverage-%p{continuous}.profraw"
    if "kcov" in os.environ.get("RUN_ANALYZER", ""):
        coverage_dir = os.path.join(cwd, "coverage")
        cmd = [
            "kcov",
            "--include-path={}".format(os.path.join(sourcedir, "src")),
            coverage_dir,
            *cmd,
        ]
    if "valgrind" in os.environ.get("RUN_ANALYZER", ""):
        cmd = ["valgrind", "--leak-check=yes", *cmd]
    try:
        return subprocess.run([*cmd, *args], cwd=cwd, env=env, **kwargs)
    except subprocess.CalledProcessError:
        raise pytest.fail.Exception(
            "running command failed: {cmd} {args}".format(
                cmd=" ".join(cmd), args=" ".join(args)
            )
        ) from None


def check_output(*args, **kwargs):
    stdout = run(*args, check=True, stdout=subprocess.PIPE, **kwargs).stdout
    # capturing stdout on windows actually encodes "\n" as "\r\n", which we
    # revert, because it messes with envelope decoding
    stdout = stdout.replace(b"\r\n", b"\n")
    return stdout


# Adapted from: https://raw.githubusercontent.com/getsentry/sentry-python/276acae955ee13f7ac3a7728003626eff6d943a8/sentry_sdk/envelope.py


class Envelope(object):
    def __init__(
        self,
        headers=None,  # type: Optional[Dict[str, str]]
        items=None,  # type: Optional[List[Item]]
    ):
        # type: (...) -> None
        if headers is not None:
            headers = dict(headers)
        self.headers = headers or {}
        if items is None:
            items = []
        else:
            items = list(items)
        self.items = items

    def get_event(self):
        # type: (...) -> Optional[Event]
        for item in self.items:
            event = item.get_event()
            if event is not None:
                return event
        return None

    def __iter__(self):
        # type: (...) -> Iterator[Item]
        return iter(self.items)

    @classmethod
    def deserialize_from(
        cls, f  # type: Any
    ):
        # type: (...) -> Envelope
        headers = json.loads(f.readline())
        items = []
        while 1:
            item = Item.deserialize_from(f)
            if item is None:
                break
            items.append(item)
        return cls(headers=headers, items=items)

    @classmethod
    def deserialize(
        cls, bytes  # type: bytes
    ):
        # type: (...) -> Envelope
        return cls.deserialize_from(io.BytesIO(bytes))

    def __repr__(self):
        # type: (...) -> str
        return "<Envelope headers=%r items=%r>" % (self.headers, self.items)


class PayloadRef(object):
    def __init__(
        self,
        bytes=None,  # type: Optional[bytes]
        json=None,  # type: Optional[Any]
    ):
        # type: (...) -> None
        self.json = json
        self.bytes = bytes

    def __repr__(self):
        # type: (...) -> str
        return "<Payload bytes=%r json=%r>" % (self.bytes, self.json)


class Item(object):
    def __init__(
        self,
        payload,  # type: Union[bytes, text_type, PayloadRef]
        headers=None,  # type: Optional[Dict[str, str]]
    ):
        if headers is not None:
            headers = dict(headers)
        elif headers is None:
            headers = {}
        self.headers = headers
        if isinstance(payload, bytes):
            payload = PayloadRef(bytes=payload)
        else:
            payload = payload

        self.payload = payload

    def get_event(self):
        # type: (...) -> Optional[Event]
        if self.headers.get("type") == "event" and self.payload.json is not None:
            return self.payload.json
        return None

    @classmethod
    def deserialize_from(
        cls, f  # type: Any
    ):
        # type: (...) -> Optional[Item]
        line = f.readline().rstrip()
        if not line:
            return None
        headers = json.loads(line)
        length = headers["length"]
        payload = f.read(length)
        if headers.get("type") == "event" or headers.get("type") == "session":
            rv = cls(headers=headers, payload=PayloadRef(json=json.loads(payload)))
        else:
            rv = cls(headers=headers, payload=payload)
        f.readline()
        return rv

    @classmethod
    def deserialize(
        cls, bytes  # type: bytes
    ):
        # type: (...) -> Optional[Item]
        return cls.deserialize_from(io.BytesIO(bytes))

    def __repr__(self):
        # type: (...) -> str
        return "<Item headers=%r payload=%r>" % (self.headers, self.payload,)
