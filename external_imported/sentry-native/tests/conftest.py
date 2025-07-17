import json
import os
import pytest
import re
import statistics
import sys
from . import run
from .cmake import CMake


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


def pytest_addoption(parser):
    parser.addoption(
        "--with_crashpad_wer",
        action="store_true",
        help="Enables tests for the crashpad WER module on Windows",
    )
    parser.addoption(
        "--benchmark_out",
        action="store",
        help="Output file for benchmark results",
    )


def pytest_runtest_setup(item):
    if "with_crashpad_wer" in item.keywords and not item.config.getoption(
        "--with_crashpad_wer"
    ):
        pytest.skip("need --with_crashpad_wer to run this test")


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "with_crashpad_wer: mark test to only run when WER testing is enabled",
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
