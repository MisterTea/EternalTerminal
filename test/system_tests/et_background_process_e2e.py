#!/usr/bin/env python3
"""System test for Issue #448: ET session teardown with running background process.

Verifies that sending a long-running process to the background (e.g. `sleep 100 &`)
and then exiting the foreground shell via Ctrl+D (EOF, `\\x04`) causes the ET session
to end promptly without waiting for the background process to complete.
"""

from __future__ import annotations

import argparse
import fcntl
import os
import pty
import re
import select
import signal
import socket
import subprocess
import sys
import tempfile
import termios
import time
from pathlib import Path

SKIP = 77

# Must be produced by the remote shell as its own output line. Local PTY echo
# of the typed `echo …` command must not satisfy readiness.
READY_MARKER = "ET_448_READY"


def skip(reason: str) -> None:
    print(f"SKIP: {reason}", flush=True)
    raise SystemExit(SKIP)


def fail(reason: str) -> None:
    print(f"FAIL: {reason}", flush=True)
    raise SystemExit(1)


def get_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def strip_terminal_noise(text: str) -> str:
    """Remove OSC/CSI sequences that shell integration may glue onto output lines."""
    # OSC: ESC ] ... BEL  or  ESC ] ... ESC \
    without_osc = re.sub(r"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)", "", text)
    # CSI / private-mode sequences (e.g. bracketed paste)
    return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", without_osc)


def line_is_exactly(accumulated: str, marker: str) -> bool:
    """True if any line equals marker after stripping CR and terminal noise.

    Exact-line matching avoids treating local PTY echo of `echo MARKER` as
    readiness. Shell-integration OSC can share a physical line with the marker
    (ST immediately followed by the echo payload), so strip that noise first.
    """
    cleaned = strip_terminal_noise(accumulated)
    for raw in cleaned.splitlines():
        if raw.strip("\r") == marker:
            return True
    return False


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Issue 448 background process teardown system test"
    )
    parser.add_argument(
        "--et",
        type=Path,
        default=Path("build/et"),
        help="Path to et client binary",
    )
    parser.add_argument(
        "--etserver",
        type=Path,
        default=Path("build/etserver"),
        help="Path to etserver binary",
    )
    parser.add_argument(
        "--etterminal",
        type=Path,
        default=Path("build/etterminal"),
        help="Path to etterminal binary",
    )
    return parser.parse_args()


def main() -> None:
    if sys.platform.startswith("win"):
        skip("PTY system test only supported on Unix")

    args = parse_args()
    et_bin = args.et.resolve()
    etserver_bin = args.etserver.resolve()
    etterminal_bin = args.etterminal.resolve()

    for binary, name in [
        (et_bin, "et"),
        (etserver_bin, "etserver"),
        (etterminal_bin, "etterminal"),
    ]:
        if not binary.is_file() or not os.access(binary, os.X_OK):
            skip(f"{name} binary not found or not executable at {binary}")

    port = get_free_port()
    bg_sleep_pid: int | None = None

    with tempfile.TemporaryDirectory(prefix="et_test_448_") as temp_dir:
        temp_path = Path(temp_dir)
        fifo_path = temp_path / "etserver.fifo"
        log_dir = temp_path / "logs"
        log_dir.mkdir(parents=True, exist_ok=True)

        # Self-contained ssh shim: execute the remote command locally so this
        # test does not need a listening sshd (localhost:22 may be refused).
        # Matches the IDPASSKEY bootstrap path used by other et system tests.
        bin_dir = temp_path / "bin"
        bin_dir.mkdir(parents=True, exist_ok=True)
        ssh_shim = bin_dir / "ssh"
        ssh_shim.write_text(
            "#!/bin/sh\n"
            "for last; do :; done\n"
            'exec /bin/sh -c "$last"\n',
            encoding="utf-8",
        )
        ssh_shim.chmod(0o755)

        server_proc: subprocess.Popen | None = None
        et_proc: subprocess.Popen | None = None
        master_fd = -1

        try:
            # 1. Start etserver
            server_cmd = [
                str(etserver_bin),
                f"--port={port}",
                f"--serverfifo={fifo_path}",
                "-l",
                str(log_dir),
                "--logtostdout",
                "--verbose=1",
            ]
            server_proc = subprocess.Popen(server_cmd)

            # Wait for etserver to listen
            server_ready = False
            deadline = time.time() + 10.0
            while time.time() < deadline:
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                        server_ready = True
                        break
                except OSError:
                    time.sleep(0.1)

            if not server_ready:
                fail(f"etserver failed to start and listen on port {port}")

            # 2. Start et inside a PTY.
            # Client flags must come before the host: anything after the host
            # positional is a remote command (see splitEtArgvAtHost).
            env = os.environ.copy()
            env["PATH"] = f"{bin_dir}:{env.get('PATH', '')}"
            env["TERM"] = "xterm-256color"
            env["SHELL"] = "/bin/sh"
            env["ET_NO_TELEMETRY"] = "YES"

            master_fd, slave_fd = pty.openpty()

            def _setup_slave() -> None:
                os.setsid()
                try:
                    fcntl.ioctl(slave_fd, termios.TIOCSCTTY, 0)
                except OSError:
                    pass

            et_cmd = [
                str(et_bin),
                f"--terminal-path={etterminal_bin}",
                f"--serverfifo={fifo_path}",
                "--no-ssh-config",
                "--logtostdout",
                "--verbose=1",
                f"127.0.0.1:{port}",
            ]

            et_proc = subprocess.Popen(
                et_cmd,
                stdin=slave_fd,
                stdout=slave_fd,
                stderr=slave_fd,
                env=env,
                close_fds=True,
                preexec_fn=_setup_slave,
            )
            os.close(slave_fd)

            # Set non-blocking on master_fd
            flags = fcntl.fcntl(master_fd, fcntl.F_GETFL)
            fcntl.fcntl(master_fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

            def pump_pty(timeout: float) -> str:
                buf = ""
                end = time.time() + timeout
                while time.time() < end:
                    remaining = max(0.0, end - time.time())
                    r, _, _ = select.select([master_fd], [], [], remaining)
                    if not r:
                        break
                    try:
                        chunk = os.read(master_fd, 4096)
                        if not chunk:
                            break
                        buf += chunk.decode("utf-8", "replace")
                    except OSError:
                        break
                return buf

            # Wait for remote shell by requiring an exact output line from echo.
            ready = False
            probe_deadline = time.time() + 30.0
            accumulated = ""
            while time.time() < probe_deadline:
                os.write(master_fd, f"echo {READY_MARKER}\n".encode())
                accumulated += pump_pty(0.5)
                if line_is_exactly(accumulated, READY_MARKER):
                    ready = True
                    print(
                        f"Remote shell ready (exact line {READY_MARKER!r})",
                        flush=True,
                    )
                    break
                if et_proc.poll() is not None:
                    fail(
                        "et exited unexpectedly during startup with code "
                        f"{et_proc.returncode}; accumulated={accumulated[-800:]!r}"
                    )

            if not ready:
                fail(
                    "Timed out waiting for remote shell ready line; "
                    f"accumulated={accumulated[-800:]!r}"
                )

            # 3. Issue #448 playbook:
            # - Send a long sleep to background with '&'
            # - Record its PID from shell output (no global pkill)
            # - Send Ctrl+D (EOF)
            # - Ensure et session ends promptly
            print("Sending HUP-immune 'sleep 100 &' and capturing PID", flush=True)
            # Interactive shells may SIGHUP background jobs on exit; trap HUP so
            # the descendant keeps the PTY slave open (the Issue #448 hang).
            os.write(
                master_fd,
                b"/bin/sh -c 'trap \"\" HUP; sleep 100 & "
                b"printf \"ET_448_BG_PID=%s\\n\" \"$!\"'\n",
            )
            pid_deadline = time.time() + 5.0
            while time.time() < pid_deadline and bg_sleep_pid is None:
                accumulated += pump_pty(0.3)
                match = re.search(
                    r"ET_448_BG_PID=(\d+)", accumulated.replace("\r", "")
                )
                if match:
                    bg_sleep_pid = int(match.group(1))
                    print(f"Background sleep pid={bg_sleep_pid}", flush=True)
                    break

            if bg_sleep_pid is None:
                fail(
                    "Could not capture background sleep PID; "
                    f"accumulated={accumulated[-800:]!r}"
                )

            print("Sending Ctrl+D (EOF)", flush=True)
            exit_start = time.time()
            os.write(master_fd, b"\x04")

            # Wait for session teardown. In shells with checkjobs (e.g. zsh),
            # the first ^D warns and a second ^D confirms exit.
            session_ended = False
            exit_deadline = time.time() + 10.0
            second_eof_sent = False

            while time.time() < exit_deadline:
                pump_pty(0.1)
                ret = et_proc.poll()
                if ret is not None:
                    duration = time.time() - exit_start
                    print(
                        f"SUCCESS: et exited with code {ret} after {duration:.2f}s "
                        f"(did not hang on background sleep pid={bg_sleep_pid})",
                        flush=True,
                    )
                    session_ended = True
                    if ret != 0:
                        fail(f"et exited with non-zero code {ret}")
                    break

                if not second_eof_sent and (time.time() - exit_start > 1.0):
                    # Send second ^D in case the shell required confirmation
                    os.write(master_fd, b"\x04")
                    second_eof_sent = True

            if not session_ended:
                fail(
                    "et failed to exit promptly after shell exited; "
                    "session blocked on background process"
                )

        finally:
            if bg_sleep_pid is not None:
                try:
                    os.kill(bg_sleep_pid, signal.SIGKILL)
                except OSError:
                    pass
                try:
                    os.waitpid(bg_sleep_pid, os.WNOHANG)
                except OSError:
                    pass
            if master_fd >= 0:
                try:
                    os.close(master_fd)
                except OSError:
                    pass
            if et_proc is not None and et_proc.poll() is None:
                et_proc.terminate()
                try:
                    et_proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    et_proc.kill()
            if server_proc is not None and server_proc.poll() is None:
                server_proc.terminate()
                try:
                    server_proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    server_proc.kill()


if __name__ == "__main__":
    main()
