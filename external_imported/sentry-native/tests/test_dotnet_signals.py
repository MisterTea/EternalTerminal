import os
import pathlib
import shutil
import subprocess
import sys
import time

import pytest

from tests import adb
from tests.assertions import wait_for
from tests.conditions import is_android, is_arm32, is_tsan, is_x86, is_asan

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
    return run_dotnet(
        tmp_path, ["dotnet", "run", "-f:net10.0", "--", "managed-exception"]
    )


def run_dotnet_unhandled_managed_exception(tmp_path):
    return run_dotnet(
        tmp_path, ["dotnet", "run", "-f:net10.0", "--", "unhandled-managed-exception"]
    )


def run_dotnet_native_crash(tmp_path):
    return run_dotnet(tmp_path, ["dotnet", "run", "-f:net10.0", "--", "native-crash"])


@pytest.mark.skipif(
    bool(
        sys.platform != "linux"
        or is_x86
        or is_arm32
        or is_asan
        or is_tsan
        or is_android
    ),
    reason="dotnet signal handling is currently only supported on 64-bit Linux without sanitizers",
)
def test_dotnet_signals_inproc(cmake):
    if shutil.which("dotnet") is None:
        pytest.skip("dotnet is not installed")

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
        # raising a signal that CLR converts to a managed exception, which is then handled by the managed code and
        # not leaked out to the native code so no crash is registered.
        dotnet_run = run_dotnet_managed_exception(tmp_path)
        dotnet_run_stdout, dotnet_run_stderr = dotnet_run.communicate()

        # the program handles the `NullReferenceException`, so the Native SDK won't register a crash.
        assert dotnet_run.returncode == 0
        assert not (
            "NullReferenceException" in dotnet_run_stderr
        ), f"Managed exception run failed.\nstdout:\n{dotnet_run_stdout}\nstderr:\n{dotnet_run_stderr}"
        database_path = project_fixture_path / ".sentry-native"
        assert database_path.exists(), "No database-path exists"
        assert not (database_path / "last_crash").exists(), "A crash was registered"
        assert_empty_run_dir(database_path)

        # this runs the dotnet program with the Native SDK and chain-at-start, when managed code raises a signal that CLR convert to an exception.
        dotnet_run = run_dotnet_unhandled_managed_exception(tmp_path)
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


def run_aot(tmp_path, args=None):
    if args is None:
        args = []
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(tmp_path) + ":" + env.get("LD_LIBRARY_PATH", "")
    return subprocess.Popen(
        [str(tmp_path / "bin/test_dotnet")] + args,
        cwd=tmp_path,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def run_aot_managed_exception(tmp_path):
    return run_aot(tmp_path, ["managed-exception"])


def run_aot_unhandled_managed_exception(tmp_path):
    return run_aot(tmp_path, ["unhandled-managed-exception"])


def run_aot_native_crash(tmp_path):
    return run_aot(tmp_path, ["native-crash"])


@pytest.mark.skipif(
    bool(
        sys.platform != "linux"
        or is_x86
        or is_arm32
        or is_asan
        or is_tsan
        or is_android
    ),
    reason="dotnet AOT signal handling is currently only supported on 64-bit Linux without sanitizers",
)
def test_aot_signals_inproc(cmake):
    if shutil.which("dotnet") is None:
        pytest.skip("dotnet is not installed")

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

        # AOT-compile the dotnet program
        subprocess.run(
            [
                "dotnet",
                "publish",
                "-f:net10.0",
                "-p:PublishAot=true",
                "-p:Configuration=Release",
                "-o",
                str(tmp_path / "bin"),
            ],
            cwd=project_fixture_path,
            check=True,
        )

        # this runs the dotnet program in AOT mode with the Native SDK and chain-at-start, and triggers a `NullReferenceException`
        # raising a signal that CLR converts to a managed exception, which is then handled by the managed code and
        # not leaked out to the native code so no crash is registered.
        dotnet_run = run_aot_managed_exception(tmp_path)
        dotnet_run_stdout, dotnet_run_stderr = dotnet_run.communicate()

        # the program handles the `NullReferenceException`, so the Native SDK won't register a crash.
        assert dotnet_run.returncode == 0
        assert not (
            "NullReferenceException" in dotnet_run_stderr
        ), f"Managed exception run failed.\nstdout:\n{dotnet_run_stdout}\nstderr:\n{dotnet_run_stderr}"
        database_path = tmp_path / ".sentry-native"
        assert database_path.exists(), "No database-path exists"
        assert not (database_path / "last_crash").exists(), "A crash was registered"
        assert_empty_run_dir(database_path)

        # this runs the dotnet program in AOT mode with the Native SDK and chain-at-start, and triggers a `NullReferenceException`
        # raising a signal that CLR converts to a managed exception, which is then not handled by the managed code but
        # leaked out to the native code so a crash is registered.
        dotnet_run = run_aot_unhandled_managed_exception(tmp_path)
        dotnet_run_stdout, dotnet_run_stderr = dotnet_run.communicate()

        # the program will fail with a `NullReferenceException`, so the Native SDK will register a crash.
        assert dotnet_run.returncode != 0
        assert (
            "NullReferenceException" in dotnet_run_stderr
        ), f"Managed exception run failed.\nstdout:\n{dotnet_run_stdout}\nstderr:\n{dotnet_run_stderr}"
        database_path = tmp_path / ".sentry-native"
        assert database_path.exists(), "No database-path exists"
        assert (database_path / "last_crash").exists()
        assert_run_dir_with_envelope(database_path)

        # this runs the dotnet program with the Native SDK and chain-at-start, when an actual native crash raises a signal
        dotnet_run = run_aot_native_crash(tmp_path)
        dotnet_run_stdout, dotnet_run_stderr = dotnet_run.communicate()

        # the program will fail with a SIGSEGV, that has been processed by the Native SDK which produced a crash envelope
        assert dotnet_run.returncode != 0
        assert (
            "crash has been captured" in dotnet_run_stderr
        ), f"Native exception run failed.\nstdout:\n{dotnet_run_stdout}\nstderr:\n{dotnet_run_stderr}"
        assert (database_path / "last_crash").exists()
        assert_run_dir_with_envelope(database_path)
    finally:
        shutil.rmtree(tmp_path / ".sentry-native", ignore_errors=True)
        shutil.rmtree(project_fixture_path / "bin", ignore_errors=True)
        shutil.rmtree(project_fixture_path / "obj", ignore_errors=True)


ANDROID_PACKAGE = "io.sentry.ndk.dotnet.signal.test"


def run_android(args=None, strategy=None, reinit=False, timeout=30):
    if args is None:
        args = []
    adb("logcat", "-c")
    adb("shell", "am", "force-stop", ANDROID_PACKAGE)
    adb(
        "shell", "run-as {} sh -c 'rm -rf files/.sentry-native'".format(ANDROID_PACKAGE)
    )
    intent_args = []
    for arg in args:
        intent_args += ["--es", "arg", arg]
    if strategy:
        intent_args += ["--es", "strategy", strategy]
    if reinit:
        intent_args += ["--es", "reinit", "true"]
    try:
        adb(
            "shell",
            "am",
            "start",
            "-W",
            "-n",
            "{}/dotnet_signal.MainActivity".format(ANDROID_PACKAGE),
            *intent_args,
            check=True,
            timeout=10,
        )
    except subprocess.TimeoutExpired:
        pass
    pid = adb(
        "shell", "pidof", ANDROID_PACKAGE, capture_output=True, text=True
    ).stdout.strip()
    wait_for(
        lambda: adb(
            "shell", "pidof", ANDROID_PACKAGE, capture_output=True, text=True
        ).returncode
        != 0,
        timeout=timeout,
    )
    logcat_args = ["logcat", "-d"]
    if pid:
        logcat_args += ["--pid=" + pid]
    return adb(*logcat_args, capture_output=True, text=True).stdout


def run_android_managed_exception(strategy=None, reinit=False):
    return run_android(["managed-exception"], strategy=strategy, reinit=reinit)


def run_android_unhandled_managed_exception(strategy=None):
    return run_android(["unhandled-managed-exception"], strategy=strategy)


def run_android_native_crash(strategy=None, reinit=False):
    return run_android(["native-crash"], strategy=strategy, reinit=reinit)


ndk_aar_path = (
    pathlib.Path(__file__).parent.parent
    / "ndk"
    / "lib"
    / "build"
    / "outputs"
    / "aar"
    / "sentry-native-ndk-release.aar"
)


@pytest.mark.skipif(
    not is_android or int(is_android) < 26,
    reason="needs Android API 26+ (tombstoned)",
)
# Mono on Android keeps using CHAIN_AT_START. Preload is the CoreCLR path,
# where sentry-native can enter the signal chain before the runtime installs
# its own handlers.
@pytest.mark.parametrize(
    "runtime,strategy",
    [
        ("mono", "chain-at-start"),
        ("coreclr", "preload"),
    ],
)
def test_android_signals_inproc(cmake, runtime, strategy):
    if shutil.which("dotnet") is None:
        pytest.skip("dotnet is not installed")

    if strategy == "preload":
        ndk_dir = pathlib.Path(__file__).parent.parent / "ndk"
        subprocess.run(["./gradlew", "assembleRelease"], cwd=ndk_dir, check=True)

    use_mono = runtime == "mono"
    is_preload = strategy == "preload"

    arch = os.environ.get("ANDROID_ARCH", "x86_64")
    rid_map = {
        "x86_64": "android-x64",
        "x86": "android-x86",
        "arm64-v8a": "android-arm64",
        "armeabi-v7a": "android-arm",
    }

    try:
        tmp_path = cmake(
            ["sentry"],
            {"SENTRY_BACKEND": "inproc", "SENTRY_TRANSPORT": "none"},
        )

        # build libcrash.so with NDK clang
        ndk_prebuilt = pathlib.Path(
            "{}/ndk/{}/toolchains/llvm/prebuilt".format(
                os.environ["ANDROID_HOME"], os.environ["ANDROID_NDK"]
            )
        )
        triples = {
            "x86_64": "x86_64-linux-android",
            "x86": "i686-linux-android",
            "arm64-v8a": "aarch64-linux-android",
            "armeabi-v7a": "armv7a-linux-androideabi",
        }
        ndk_clang = str(
            next(ndk_prebuilt.iterdir())
            / "bin"
            / "{}{}-clang".format(triples[arch], os.environ["ANDROID_API"])
        )
        native_lib_dir = project_fixture_path / "native" / arch
        native_lib_dir.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [
                ndk_clang,
                "-Wall",
                "-Wextra",
                "-fPIC",
                "-shared",
                "-Wl,-z,max-page-size=16384",
                str(project_fixture_path / "crash.c"),
                "-o",
                str(native_lib_dir / "libcrash.so"),
            ],
            check=True,
        )

        # for chain-at-start, bundle the cmake-built libsentry.so directly
        # (preload uses the AAR which brings its own libsentry.so)
        if not is_preload:
            shutil.copy2(tmp_path / "libsentry.so", native_lib_dir / "libsentry.so")

        # build and install the APK
        build_args = [
            "dotnet",
            "build",
            "-f:net10.0-android",
            "-p:RuntimeIdentifier={}".format(rid_map[arch]),
            "-p:Configuration=Release",
            "-p:UseMonoRuntime={}".format(str(use_mono).lower()),
            "-p:EnablePreviewFeatures=true",
        ]
        if is_preload:
            build_args += [
                "-p:EnablePreload=true",
                "-p:NdkAarPath={}".format(ndk_aar_path),
            ]
        subprocess.run(build_args, cwd=project_fixture_path, check=True)
        apk_dir = (
            project_fixture_path / "bin" / "Release" / "net10.0-android" / rid_map[arch]
        )
        apk_path = next(apk_dir.glob("*-Signed.apk"))
        adb("install", "-r", str(apk_path), check=True)

        def run_as(cmd, **kwargs):
            return adb(
                "shell",
                'run-as {} sh -c "{}"'.format(ANDROID_PACKAGE, cmd),
                **kwargs,
            )

        db = "files/.sentry-native"

        def file_exists(path):
            return run_as("test -f " + path, capture_output=True).returncode == 0

        def dir_exists(path):
            return run_as("test -d " + path, capture_output=True).returncode == 0

        def has_envelope():
            result = run_as(
                "find " + db + " -name '*.envelope'", capture_output=True, text=True
            )
            return bool(result.stdout.strip())

        # Preload replaces CHAIN_AT_START in the CoreCLR path. Once the runtime
        # chains native crashes back to sentry-native, the app-side handler strategy
        # should be DEFAULT rather than CHAIN_AT_START.
        app_strategy = "default-strategy" if is_preload else None

        # managed exception: handled, no crash
        logcat = run_android_managed_exception(strategy=app_strategy)
        assert not (
            "NullReferenceException" in logcat
        ), f"Managed exception leaked.\nlogcat:\n{logcat}"
        assert wait_for(lambda: dir_exists(db)), "No database-path exists"
        assert not file_exists(db + "/last_crash"), "A crash was registered"
        assert not has_envelope(), "Unexpected envelope found"

        # unhandled managed exception: the runtime calls exit(1), the native
        # SDK should not register a crash (sentry-dotnet handles this at the
        # managed layer via UnhandledExceptionRaiser)
        logcat = run_android_unhandled_managed_exception(strategy=app_strategy)
        assert (
            "NullReferenceException" in logcat
        ), f"Expected NullReferenceException.\nlogcat:\n{logcat}"
        assert wait_for(lambda: dir_exists(db)), "No database-path exists"
        assert not file_exists(db + "/last_crash"), "A crash was registered"
        assert not has_envelope(), "Unexpected envelope found"

        # native crash
        run_android_native_crash(strategy=app_strategy)
        assert wait_for(lambda: file_exists(db + "/last_crash")), "Crash marker missing"
        assert wait_for(has_envelope), "Crash envelope is missing"

        if is_preload:
            # after close/reinit, managed exceptions must still be handled by
            # the runtime (not caught as native crashes by sentry)
            logcat = run_android_managed_exception(strategy=app_strategy, reinit=True)
            assert not (
                "NullReferenceException" in logcat
            ), f"Managed exception leaked after reinit.\nlogcat:\n{logcat}"
            assert wait_for(lambda: dir_exists(db)), "No database-path exists"
            assert not file_exists(
                db + "/last_crash"
            ), "A crash was registered after reinit"
            assert not has_envelope(), "Unexpected envelope found after reinit"

            # after close/reinit, native crashes must still be captured through
            # the preloaded chain position
            run_android_native_crash(strategy=app_strategy, reinit=True)
            assert wait_for(
                lambda: file_exists(db + "/last_crash")
            ), "Crash marker missing after reinit"
            assert wait_for(has_envelope), "Crash envelope is missing after reinit"
    finally:
        shutil.rmtree(project_fixture_path / "native", ignore_errors=True)
        shutil.rmtree(project_fixture_path / "bin", ignore_errors=True)
        shutil.rmtree(project_fixture_path / "obj", ignore_errors=True)
        adb("uninstall", ANDROID_PACKAGE, check=False)
