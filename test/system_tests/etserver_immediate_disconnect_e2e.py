#!/usr/bin/env python3
"""etserver must survive a client that connects and sends nothing.

Issue #903: a probe such as ``nc -z`` makes the handshake ``readAll`` throw.
macOS logs the error and keeps listening. Linux dies while unwinding that
exception (SIGSEGV, then abort if the crash logger is still installed).
"""

from __future__ import annotations

import argparse
import os
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path

SKIP = 77


def skip(reason: str) -> None:
    print(f"SKIP: {reason}", flush=True)
    raise SystemExit(SKIP)


def fail(reason: str) -> None:
    print(f"FAIL: {reason}", flush=True)
    raise SystemExit(1)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def read_log(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return f"<could not read {path}: {exc}>"


def connect_and_close(port: int) -> None:
    """Open a TCP connection and drop it before any handshake bytes."""
    with socket.create_connection(("127.0.0.1", port), timeout=2):
        pass


def stop(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    os.killpg(proc.pid, signal.SIGTERM)
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait(timeout=3)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Issue 903: etserver survives a connect-and-close probe"
    )
    parser.add_argument("--etserver", type=Path, required=True)
    args = parser.parse_args()
    etserver = args.etserver.resolve()
    if not etserver.is_file():
        skip(f"{etserver} is not built")

    work = Path(tempfile.mkdtemp(prefix=f"et903_{os.getpid()}_"))
    log_path = work / "etserver.log"
    fifo = work / "server.fifo"
    log_dir = work / "logs"
    log_dir.mkdir()
    port = free_port()
    log_file = log_path.open("w", encoding="utf-8")
    proc = subprocess.Popen(
        [
            str(etserver),
            "--port",
            str(port),
            "--bindip",
            "127.0.0.1",
            "--serverfifo",
            str(fifo),
            "--logdir",
            str(log_dir),
            "--logtostdout",
            "--telemetry=false",
            "-v",
            "1",
        ],
        stdout=log_file,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    try:
        deadline = time.time() + 10
        while time.time() < deadline:
            if proc.poll() is not None:
                fail(
                    f"etserver exited {proc.returncode} during startup\n"
                    + read_log(log_path)
                )
            try:
                connect_and_close(port)
                break
            except OSError:
                time.sleep(0.05)
        else:
            fail("etserver did not accept a connection\n" + read_log(log_path))

        # The handler thread throws as soon as the peer closes. Give that
        # thread time to either log the error or take the process down.
        time.sleep(0.5)
        if proc.poll() is not None:
            fail(
                "etserver died after a client connected and closed without "
                f"sending data (exit {proc.returncode})\n" + read_log(log_path)
            )

        log = read_log(log_path)
        if "Error handling new client" not in log:
            fail(
                "etserver stayed up but did not log the failed handshake\n" + log
            )

        try:
            connect_and_close(port)
        except OSError as exc:
            fail(f"etserver stopped accepting after the first probe: {exc}\n" + log)
        time.sleep(0.5)
        if proc.poll() is not None:
            fail(
                "etserver died on a second connect-and-close "
                f"(exit {proc.returncode})\n" + read_log(log_path)
            )
        print("PASS: etserver survived a connect-and-close probe", flush=True)
    finally:
        log_file.close()
        stop(proc)


if __name__ == "__main__":
    main()
