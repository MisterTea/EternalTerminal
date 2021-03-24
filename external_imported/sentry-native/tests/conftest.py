import os
import pytest
import re
from . import run
from .cmake import CMake


def enumerate_unittests():
    regexp = re.compile("XX\((.*?)\)")
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
