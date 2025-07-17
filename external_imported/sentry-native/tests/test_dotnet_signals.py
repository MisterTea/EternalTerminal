import os
import pathlib
import shutil
import subprocess
import sys

import pytest

project_fixture_path = pathlib.Path("tests/fixtures/dotnet_signal")


def assert_empty_run_dir(database_path):
    run_dirs = [d for d in database_path.glob("*.run") if d.is_dir()]
    assert (
        len(run_dirs) == 1
    ), f"Expected exactly one .run directory, found {len(run_dirs)}"

    run_dir = run_dirs[0]
    assert not any(run_dir.iterdir()), f"The directory {run_dir} is not empty"


def assert_run_dir_with_envelope(database_path):
    run_dirs = [d for d in database_path.glob("*.run") if d.is_dir()]
    assert (
        len(run_dirs) == 1
    ), f"Expected exactly one .run directory, found {len(run_dirs)}"

    run_dir = run_dirs[0]
    crash_envelopes = [f for f in run_dir.glob("*.envelope") if f.is_file()]
    assert len(crash_envelopes) > 0, f"Crash envelope is missing"
    assert (
        len(crash_envelopes) == 1
    ), f"There is more than one crash envelope ({len(crash_envelopes)}"


def run_dotnet(tmp_path, args):
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(tmp_path) + ":" + env.get("LD_LIBRARY_PATH", "")
    return subprocess.Popen(
        args,
        cwd=str(project_fixture_path),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def run_dotnet_managed_exception(tmp_path):
    return run_dotnet(tmp_path, ["dotnet", "run"])


def run_dotnet_native_crash(tmp_path):
    return run_dotnet(tmp_path, ["dotnet", "run", "native-crash"])


skip_condition = (
    sys.platform != "linux"
    or bool(os.environ.get("TEST_X86"))
    or "asan" in os.environ.get("RUN_ANALYZER", "")
)


@pytest.mark.skipif(
    skip_condition,
    reason="dotnet signal handling is currently only supported on 64-bit Linux",
)
def test_dotnet_signals_inproc(cmake):
    try:
        # build native client library with inproc and the example for crash dumping
        tmp_path = cmake(
            ["sentry"],
            {"SENTRY_BACKEND": "inproc", "SENTRY_TRANSPORT": "none"},
        )

        # build the crashing native library
        subprocess.run(
            [
                "gcc",
                "-Wall",
                "-Wextra",
                "-fPIC",
                "-shared",
                str(project_fixture_path / "crash.c"),
                "-o",
                str(tmp_path / "libcrash.so"),
            ],
            check=True,
        )

        # this runs the dotnet program with the Native SDK and chain-at-start, when managed code raises a signal that CLR convert to an exception.
        dotnet_run = run_dotnet_managed_exception(tmp_path)
        dotnet_run_stdout, dotnet_run_stderr = dotnet_run.communicate()

        # the program will fail with a `NullReferenceException`, but the Native SDK won't register a crash.
        assert dotnet_run.returncode != 0
        assert (
            "NullReferenceException" in dotnet_run_stderr
        ), f"Managed exception run failed.\nstdout:\n{dotnet_run_stdout}\nstderr:\n{dotnet_run_stderr}"
        database_path = project_fixture_path / ".sentry-native"
        assert database_path.exists(), "No database-path exists"
        assert not (database_path / "last_crash").exists(), "A crash was registered"
        assert_empty_run_dir(database_path)

        # this runs the dotnet program with the Native SDK and chain-at-start, when an actual native crash raises a signal
        dotnet_run = run_dotnet_native_crash(tmp_path)
        dotnet_run_stdout, dotnet_run_stderr = dotnet_run.communicate()

        # the program will fail with a SIGSEGV, that has been processed by the Native SDK which produced a crash envelope
        assert dotnet_run.returncode != 0
        assert (
            "crash has been captured" in dotnet_run_stderr
        ), f"Native exception run failed.\nstdout:\n{dotnet_run_stdout}\nstderr:\n{dotnet_run_stderr}"
        assert (database_path / "last_crash").exists()
        assert_run_dir_with_envelope(database_path)
    finally:
        shutil.rmtree(project_fixture_path / ".sentry-native", ignore_errors=True)
        shutil.rmtree(project_fixture_path / "bin", ignore_errors=True)
        shutil.rmtree(project_fixture_path / "obj", ignore_errors=True)
