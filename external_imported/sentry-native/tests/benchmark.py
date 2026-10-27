import os
import shutil

import pytest

from . import make_dsn, run


def run_benchmark(target, backend, cmake, httpserver, gbenchmark, label):
    tmp_path = cmake(
        ["sentry_benchmark"],
        {
            "SENTRY_BACKEND": backend,
            "SENTRY_BUILD_BENCHMARKS": "ON",
            "CMAKE_BUILD_TYPE": "Release",
        },
    )

    env = dict(os.environ, SENTRY_DSN=make_dsn(httpserver))
    benchmark_out = tmp_path / "benchmark.json"

    for i in range(6):
        # make sure we are isolated from previous runs
        shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)

        run(
            tmp_path,
            "sentry_benchmark",
            [f"--benchmark_filter={target}", f"--benchmark_out={benchmark_out}"],
            env=env,
        )

        # ignore warmup run
        if i > 0:
            gbenchmark(benchmark_out, label)


@pytest.mark.parametrize("backend", ["inproc", "breakpad", "crashpad"])
def test_benchmark_init(backend, cmake, httpserver, gbenchmark):
    run_benchmark(
        "init", backend, cmake, httpserver, gbenchmark, f"SDK init ({backend})"
    )


@pytest.mark.parametrize("backend", ["inproc", "breakpad", "crashpad"])
def test_benchmark_backend(backend, cmake, httpserver, gbenchmark):
    run_benchmark(
        "backend",
        backend,
        cmake,
        httpserver,
        gbenchmark,
        f"Backend startup ({backend})",
    )
