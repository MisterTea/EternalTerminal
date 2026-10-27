import contextlib
import os
import socket
import subprocess
import time

import psutil

import pytest

from tests.assertions import assert_no_proxy_request, wait_for, wait_for_stdout


@contextlib.contextmanager
def closed_port():
    """Bind a port and hold it open without listening.
    Connections are guaranteed to be refused, and no other process can claim it."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        yield s.getsockname()[1]


def setup_proxy_env_vars(port):
    os.environ["http_proxy"] = f"http://127.0.0.1:{port}"
    os.environ["https_proxy"] = f"http://127.0.0.1:{port}"


def cleanup_proxy_env_vars():
    os.environ.pop("http_proxy", None)
    os.environ.pop("https_proxy", None)


def _get_process_tree(proc):
    """Return a list of psutil.Process for proc and all its descendants."""
    procs = [proc]
    try:
        procs.extend(proc.children(recursive=True))
    except (psutil.NoSuchProcess, psutil.AccessDenied):
        pass
    return procs


def _discover_listening_port(process, timeout=10):
    """Use psutil to discover which port the process (or any of its children)
    is listening on.  On Windows, pip-installed mitmdump is a launcher that
    spawns Python child processes, so the actual listener lives in a
    descendant, not the top-level PID."""
    deadline = time.monotonic() + timeout
    proc = psutil.Process(process.pid)
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"mitmdump exited with code {process.returncode} before listening"
            )
        # Collect the process and all its children (pip-installed mitmdump on
        # Windows spawns child python.exe processes that do the actual work).
        tree = _get_process_tree(proc)

        listeners = []
        for p in tree:
            try:
                listeners.extend(
                    conn
                    for conn in p.net_connections(kind="tcp")
                    if conn.status == psutil.CONN_LISTEN
                )
            except (psutil.NoSuchProcess, psutil.AccessDenied, OSError):
                continue

        if listeners:
            assert (
                len(listeners) == 1
            ), f"Expected mitmdump to listen on exactly one port, got: {listeners}"
            return listeners[0].laddr.port
        time.sleep(0.2)
    raise TimeoutError(
        f"mitmdump (pid {process.pid}) did not start listening within {timeout}s"
    )


def start_mitmdump(
    proxy_type, proxy_auth: str = None, listen_host: str = "127.0.0.1", retries: int = 3
):
    """Start mitmdump on a free port. Returns (process, port).
    Retries up to `retries` times if mitmdump fails to start listening."""
    for attempt in range(1, retries + 1):
        proxy_command = [
            "mitmdump",
            "--set",
            f"listen_host={listen_host}",
            "--listen-port",
            "0",
        ]

        if proxy_type == "socks5-proxy":
            proxy_command += ["--mode", "socks5"]

        if proxy_auth:
            proxy_command += ["-v", "--proxyauth", proxy_auth]

        proxy_env = os.environ.copy()
        proxy_env["PYTHONUNBUFFERED"] = "1"

        proxy_process = subprocess.Popen(
            proxy_command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=proxy_env,
        )

        try:
            port = _discover_listening_port(proxy_process)
            return proxy_process, port
        except (TimeoutError, RuntimeError) as e:
            proxy_process.kill()
            proxy_process.wait()
            if attempt < retries:
                print(f"mitmdump attempt {attempt}/{retries} failed, retrying: {e}")
                continue
            else:
                pytest.fail(str(e))
        except Exception:
            proxy_process.terminate()
            proxy_process.wait()
            raise
    pytest.fail("start_mitmdump: all retries exhausted")


def proxy_test_finally(
    expected_httpserver_logsize,
    httpserver,
    proxy_process,
    proxy_log_assert=assert_no_proxy_request,
    expected_proxy_logsize=None,
    timeout=10,
):
    if expected_proxy_logsize is None:
        expected_proxy_logsize = expected_httpserver_logsize

    if proxy_process:
        try:
            # Give mitmdump some time to get a response from the mock server
            assert wait_for(
                lambda: len(httpserver.log) >= expected_httpserver_logsize, timeout
            )

            if expected_proxy_logsize != 0:
                # request passed through successfully
                wait_for_stdout(
                    proxy_process,
                    lambda text: "POST" in text and "200 OK" in text,
                    timeout,
                )
        finally:
            proxy_process.terminate()
            proxy_process.wait(timeout=timeout)

        if expected_proxy_logsize == 0:
            # don't expect any incoming requests to make it through the proxy
            stdout_bytes, _ = proxy_process.communicate(timeout=timeout)
            stdout = stdout_bytes.decode("utf-8", errors="replace")
            proxy_log_assert(stdout)
    assert len(httpserver.log) == expected_httpserver_logsize
