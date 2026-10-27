import json
import os
import subprocess
import pytest
import re
import statistics
import sys
import socket
import struct
import threading
from datetime import datetime, timedelta, UTC
from . import adb, run
from .cmake import CMake
from .conditions import has_sccache, is_android
import tests

LABEL = "label"
TIME_UNIT = "time_unit"
REAL_TIME = "real_time"
CPU_TIME = "cpu_time"

gbenchmarks = {}


def enumerate_unittests():
    regexp = re.compile(r"XX\((.*?)\)")
    # TODO: actually generate the `tests.inc` file with python
    curdir = os.path.dirname(os.path.realpath(__file__))
    with open(os.path.join(curdir, "unit/tests.inc"), "r") as testsfile:
        for line in testsfile:
            match = regexp.match(line)
            if match:
                yield match.group(1)


def pytest_generate_tests(metafunc):
    if "unittest" in metafunc.fixturenames:
        metafunc.parametrize("unittest", enumerate_unittests())


@pytest.fixture(scope="session")
def cmake(tmp_path_factory):
    cmake = CMake(tmp_path_factory)

    yield cmake.compile

    cmake.destroy()


@pytest.fixture(scope="session")
def unreachable_dsn():
    """A DSN that instantly rejects connections.

    On Windows, connecting to a closed port (e.g. 127.0.0.1:19999) causes
    WinHTTP to wait ~2s before failing. This fixture instead runs a TCP
    server that accepts and immediately RSTs the connection, giving the
    SDK an instant connection failure.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    sock.listen(5)
    port = sock.getsockname()[1]

    def reject_loop():
        while True:
            try:
                conn, _ = sock.accept()
                conn.setsockopt(
                    socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0)
                )
                conn.close()
            except OSError:
                break

    thread = threading.Thread(target=reject_loop, daemon=True)
    thread.start()

    yield f"http://uiaeosnrtdy@127.0.0.1:{port}/123456"

    sock.close()


def _get_clock_offset():
    """Measure clock offset between host and Android device."""
    if not is_android:
        return timedelta(0)
    try:
        before = datetime.now(UTC)
        result = adb("shell", "date", "+%s", capture_output=True, text=True)
        after = datetime.now(UTC)
        device_time = datetime.fromtimestamp(int(result.stdout.strip()), tz=UTC)
        host_time = before + (after - before) / 2
        return device_time - host_time
    except (KeyError, ValueError, OSError):
        return timedelta(0)


@pytest.fixture(autouse=True)
def _record_test_start():
    offset = _get_clock_offset()
    tests.now = lambda: datetime.now(UTC) + offset
    tests.test_start = tests.now()


def pytest_addoption(parser):
    parser.addoption(
        "--with_wer",
        action="store_true",
        help="Enables tests for the native and crashpad WER modules on Windows",
    )
    parser.addoption(
        "--benchmark_out",
        action="store",
        help="Output file for benchmark results",
    )


def pytest_runtest_setup(item):
    if "with_wer" in item.keywords and not item.config.getoption("--with_wer"):
        pytest.skip("need --with_wer to run this test")


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "with_wer: mark test to only run when WER testing is enabled",
    )


@pytest.fixture
def gbenchmark():
    def _load(json_path, label, test_name=None):
        if test_name is None:
            test_name = os.environ.get("PYTEST_CURRENT_TEST").split(" ")[0]

        with open(json_path, "r") as f:
            data = json.load(f)

        if test_name not in gbenchmarks:
            gbenchmarks[test_name] = {
                LABEL: label,
                TIME_UNIT: "",
                REAL_TIME: [],
                CPU_TIME: [],
            }

        for benchmark in data["benchmarks"]:
            if benchmark.get("skipped", False) == True:
                pytest.skip(benchmark.get("skip_message", "skipped"))
                break
            gbenchmarks[test_name][TIME_UNIT] = benchmark[TIME_UNIT]
            gbenchmarks[test_name][REAL_TIME].append(benchmark[REAL_TIME])
            gbenchmarks[test_name][CPU_TIME].append(benchmark[CPU_TIME])

    return _load


def _get_benchmark(name, separator):
    data = gbenchmarks.get(name)
    if data is None:
        return None

    unit = data[TIME_UNIT]
    real_time = data[REAL_TIME] if data[REAL_TIME] else []
    cpu_time = data[CPU_TIME] if data[CPU_TIME] else []

    extra = [
        f"Min {min(real_time):.3f}{unit}",
        f"Max {max(real_time):.3f}{unit}",
        f"Mean {statistics.mean(real_time):.3f}{unit}",
        f"StdDev {statistics.stdev(real_time):.3f}{unit}",
        f"Median {statistics.median(real_time):.3f}{unit}",
        f"CPU {statistics.mean(cpu_time):.3f}{unit}" if sys.platform != "win32" else "",
    ]

    return {
        "name": data[LABEL],
        "unit": unit,
        "value": statistics.median(real_time),
        "extra": separator.join(e for e in extra if e),
    }


def pytest_report_teststatus(report, config):
    if report.when == "call" and report.passed:
        benchmark = _get_benchmark(report.nodeid, ", ")
        if benchmark:
            return (
                "passed",
                None,
                f"PASSED\n{benchmark['extra']}",
            )
    return None


def pytest_sessionfinish(session, exitstatus):
    json_path = session.config.getoption("--benchmark_out")
    if json_path:
        with open(json_path, "w") as f:
            benchmarks = [_get_benchmark(name, "\n") for name in gbenchmarks.keys()]
            json.dump(benchmarks, f, indent=2)


def pytest_sessionstart(session):
    if has_sccache:
        subprocess.run(["sccache", "--zero-stats"], capture_output=True)


def pytest_terminal_summary(terminalreporter):
    if has_sccache:
        result = subprocess.run(
            ["sccache", "--show-stats"], capture_output=True, text=True
        )
        if result.stdout:
            terminalreporter.section("sccache stats")
            terminalreporter.write(result.stdout)
