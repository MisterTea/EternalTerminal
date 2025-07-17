import os
import socket
import subprocess
import time

import pytest

from tests import assert_no_proxy_request


def setup_proxy_env_vars(port):
    os.environ["http_proxy"] = f"http://localhost:{port}"
    os.environ["https_proxy"] = f"http://localhost:{port}"


def cleanup_proxy_env_vars():
    del os.environ["http_proxy"]
    del os.environ["https_proxy"]


def is_proxy_running(host, port):
    try:
        with socket.create_connection((host, port), timeout=1):
            return True
    except ConnectionRefusedError:
        return False


def start_mitmdump(proxy_type, proxy_auth: str = None):
    # start mitmdump from terminal
    proxy_process = None
    if proxy_type == "http-proxy":
        proxy_command = ["mitmdump"]
        if proxy_auth:
            proxy_command += ["-v", "--proxyauth", proxy_auth]
        proxy_process = subprocess.Popen(
            proxy_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        time.sleep(5)  # Give mitmdump some time to start
        if not is_proxy_running("localhost", 8080):
            pytest.fail("mitmdump (HTTP) did not start correctly")
    elif proxy_type == "socks5-proxy":
        proxy_command = ["mitmdump", "--mode", "socks5"]
        if proxy_auth:
            proxy_command += ["-v", "--proxyauth", proxy_auth]
        proxy_process = subprocess.Popen(
            proxy_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        time.sleep(5)  # Give mitmdump some time to start
        if not is_proxy_running("localhost", 1080):
            pytest.fail("mitmdump (SOCKS5) did not start correctly")
    return proxy_process


def proxy_test_finally(
    expected_httpserver_logsize,
    httpserver,
    proxy_process,
    proxy_log_assert=assert_no_proxy_request,
    expected_proxy_logsize=None,
):
    if expected_proxy_logsize is None:
        expected_proxy_logsize = expected_httpserver_logsize

    if proxy_process:
        # Give mitmdump some time to get a response from the mock server
        time.sleep(0.5)
        proxy_process.terminate()
        proxy_process.wait()
        stdout, stderr = proxy_process.communicate()
        if expected_proxy_logsize == 0:
            # don't expect any incoming requests to make it through the proxy
            proxy_log_assert(stdout)
        else:
            # request passed through successfully
            assert "POST" in stdout and "200 OK" in stdout
    assert len(httpserver.log) == expected_httpserver_logsize
