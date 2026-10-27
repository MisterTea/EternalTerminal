#!/usr/bin/env python3
"""Headless et + tmux -CC regression for a journald emerg wall.

journald writes emerg broadcasts onto every utmp tty. etterminal registers
its pty, so the text lands in the same byte stream as tmux -CC. A line that
does not start with '%' makes iTerm2 drop the session. This drives et over a
PTY, writes the broadcast the way wall(1) does, and checks that the control
stream stays intact. Exit 77 when sshd, tmux, or the binaries are missing.
"""

from __future__ import annotations

import argparse
import os
import pty
import select
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Optional

SKIP = 77
MARKER = "WALL_INJECT_856"
AFTER = "AFTER_WALL_856"
DCS = "\x1bP1000p"
ST = "\x1b\\"


def skip(reason: str) -> None:
    print(f"SKIP: {reason}", flush=True)
    raise SystemExit(SKIP)


def fail(reason: str) -> None:
    print(f"FAIL: {reason}", flush=True)
    raise SystemExit(1)


def unused_port() -> int:
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = int(sock.getsockname()[1])
    sock.close()
    return port


def find_tmux() -> Path:
    found = shutil.which("tmux")
    if found:
        return Path(found)
    for candidate in ("/opt/homebrew/bin/tmux", "/usr/local/bin/tmux"):
        if Path(candidate).is_file():
            return Path(candidate)
    skip("tmux is not installed")
    raise AssertionError("unreachable")


def is_control_line(line: str) -> bool:
    body = line[len(DCS) :] if line.startswith(DCS) else line
    if body.startswith("%"):
        return True
    return body == ST


def raw_marker_lines(text: str) -> list[str]:
    bad = []
    for line in text.splitlines():
        if MARKER in line and not is_control_line(line):
            bad.append(line)
    return bad


class PrivateSshd:
    def __init__(self, work: Path) -> None:
        self.work = work
        self.port = unused_port()
        self.proc: Optional[subprocess.Popen] = None
        self.key = work / "user"

    def start(self) -> None:
        subprocess.check_call(
            ["ssh-keygen", "-t", "ed25519", "-f", str(self.work / "host"), "-N", ""],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        subprocess.check_call(
            ["ssh-keygen", "-t", "ed25519", "-f", str(self.key), "-N", ""],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        auth = self.work / "authorized_keys"
        auth.write_text((self.work / "user.pub").read_text(encoding="utf-8"))
        os.chmod(auth, 0o600)
        cfg = self.work / "sshd_config"
        cfg.write_text(
            "\n".join(
                [
                    f"Port {self.port}",
                    "ListenAddress 127.0.0.1",
                    f"HostKey {self.work / 'host'}",
                    f"PidFile {self.work / 'sshd.pid'}",
                    f"AuthorizedKeysFile {auth}",
                    "PasswordAuthentication no",
                    "KbdInteractiveAuthentication no",
                    "ChallengeResponseAuthentication no",
                    "PubkeyAuthentication yes",
                    "UsePAM no",
                    "StrictModes no",
                    "PermitRootLogin no",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        sshd = shutil.which("sshd") or "/usr/sbin/sshd"
        if not Path(sshd).is_file():
            skip("sshd is not available")
        self.proc = subprocess.Popen(
            [sshd, "-f", str(cfg), "-D", "-e"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.time() + 3
        while time.time() < deadline:
            if self.proc.poll() is not None:
                err = ""
                if self.proc.stderr is not None:
                    err = self.proc.stderr.read()
                skip(f"private sshd failed to start: {err.strip()}")
            try:
                probe = socket.create_connection(("127.0.0.1", self.port), 0.2)
                probe.close()
                break
            except OSError:
                time.sleep(0.05)
        else:
            skip("private sshd did not listen")
        probe = subprocess.run(
            [
                "ssh",
                "-i",
                str(self.key),
                "-o",
                "IdentitiesOnly=yes",
                "-o",
                "StrictHostKeyChecking=no",
                "-o",
                "UserKnownHostsFile=/dev/null",
                "-o",
                "BatchMode=yes",
                "-p",
                str(self.port),
                "127.0.0.1",
                "echo ok",
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
        if probe.returncode != 0 or "ok" not in probe.stdout:
            skip(f"private sshd rejected the test key: {probe.stderr.strip()}")

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)


def session_tty(fifo: str) -> Optional[str]:
    out = subprocess.check_output(
        ["ps", "-axww", "-o", "pid=,ppid=,tty=,command="], text=True
    )
    rows = []
    parents = set()
    for line in out.splitlines():
        parts = line.split(None, 3)
        if len(parts) < 4:
            continue
        pid, ppid, tty, cmd = int(parts[0]), int(parts[1]), parts[2], parts[3]
        rows.append((pid, ppid, tty))
        if "etterminal" in cmd and fifo in cmd:
            parents.add(pid)
    if not parents:
        return None
    family = set(parents)
    changed = True
    while changed:
        changed = False
        for pid, ppid, _tty in rows:
            if ppid in family and pid not in family:
                family.add(pid)
                changed = True
    for pid, _ppid, tty in rows:
        if pid not in family or tty in ("??", "?", "-"):
            continue
        path = tty if tty.startswith("/") else "/dev/" + tty
        if os.path.exists(path):
            return path
    return None


def wall_bytes() -> bytes:
    # Same shape systemd's wall() writes for an emerg journal line.
    text = (
        "\r\n"
        "Broadcast message from systemd-journald@testhost "
        "(Wed 2026-09-23 23:04:10 UTC):\r\n"
        "\r\n"
        f"journald-test[4556]: Test message at priority: emerg {MARKER}\r\n"
        "\r\n"
    )
    return text.encode()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--et", type=Path, required=True)
    parser.add_argument("--etserver", type=Path, required=True)
    parser.add_argument("--etterminal", type=Path, required=True)
    args = parser.parse_args()
    args.et = args.et.resolve()
    args.etserver = args.etserver.resolve()
    args.etterminal = args.etterminal.resolve()
    for binary in (args.et, args.etserver, args.etterminal):
        if not binary.is_file():
            skip(f"{binary} is not built")
    tmux = find_tmux()

    work = Path(tempfile.mkdtemp(prefix="et_wall_cc_"))
    sshd = PrivateSshd(work)
    server: Optional[subprocess.Popen] = None
    client: Optional[subprocess.Popen] = None
    master_fd = -1
    text = ""
    socket_name = f"etwall{os.getpid()}"
    try:
        sshd.start()
        fifo = work / "server.fifo"
        log_dir = work / "logs"
        log_dir.mkdir()
        port = unused_port()
        server = subprocess.Popen(
            [
                str(args.etserver),
                "--port",
                str(port),
                "--serverfifo",
                str(fifo),
                "-l",
                str(log_dir),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.time() + 8
        while time.time() < deadline:
            if server.poll() is not None:
                fail(f"etserver exited {server.returncode}")
            try:
                probe = socket.create_connection(("127.0.0.1", port), 0.2)
                probe.close()
                break
            except OSError:
                time.sleep(0.05)
        else:
            fail("etserver did not listen")

        master_fd, slave_fd = pty.openpty()
        env = os.environ.copy()
        env["TERM"] = "xterm-256color"
        client = subprocess.Popen(
            [
                str(args.et),
                "--serverfifo",
                str(fifo),
                "--terminal-path",
                str(args.etterminal),
                "--logdir",
                str(log_dir),
                "--ssh-option",
                f"Port={sshd.port}",
                "--ssh-option",
                f"IdentityFile={sshd.key}",
                "--ssh-option",
                "IdentitiesOnly=yes",
                "--ssh-option",
                "StrictHostKeyChecking=no",
                "--ssh-option",
                "UserKnownHostsFile=/dev/null",
                "--ssh-option",
                "BatchMode=yes",
                f"127.0.0.1:{port}",
            ],
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            env=env,
            close_fds=True,
        )
        os.close(slave_fd)

        def pump(timeout: float) -> None:
            nonlocal text
            end = time.time() + timeout
            while time.time() < end:
                remaining = max(0.0, end - time.time())
                ready, _, _ = select.select([master_fd], [], [], min(0.1, remaining))
                if not ready:
                    continue
                try:
                    chunk = os.read(master_fd, 65536)
                except OSError:
                    return
                if not chunk:
                    return
                text += chunk.decode("utf-8", "replace")

        def send(line: str) -> None:
            data = (line + "\n").encode()
            view = memoryview(data)
            while view:
                n = os.write(master_fd, view)
                if n <= 0:
                    fail("failed to write to the et client pty")
                view = view[n:]

        deadline = time.time() + 20
        while time.time() < deadline and "$" not in text and "%" not in text:
            pump(0.2)
            if client.poll() is not None:
                fail(f"et exited before a prompt: {text[-500:]!r}")
        if "$" not in text and "%" not in text:
            fail(f"timed out waiting for a shell prompt: {text[-500:]!r}")

        send(f"exec {tmux} -L {socket_name} -CC new-session -s wall")
        deadline = time.time() + 15
        while "%session-changed" not in text and time.time() < deadline:
            pump(0.2)
            if client.poll() is not None:
                fail(f"et exited during tmux -CC startup: {text[-800:]!r}")
        if "%session-changed" not in text:
            fail(f"tmux -CC did not start: {text[-800:]!r}")

        tty = None
        deadline = time.time() + 5
        while tty is None and time.time() < deadline:
            tty = session_tty(str(fifo))
            if tty is None:
                time.sleep(0.1)
        if tty is None:
            fail(f"could not find the etterminal pty for {fifo}")

        flags = os.O_WRONLY | os.O_NONBLOCK
        if hasattr(os, "O_NOCTTY"):
            flags |= os.O_NOCTTY
        fd = os.open(tty, flags)
        try:
            os.write(fd, wall_bytes())
        finally:
            os.close(fd)
        pump(1.5)

        leaked = raw_marker_lines(text)
        if leaked:
            fail(
                "wall text reached the tmux -CC client outside a % line:\n"
                + "\n".join(leaked)
                + f"\n--- stream ---\n{text[-1500:]}"
            )

        send("display-message -p " + AFTER)
        deadline = time.time() + 8
        while AFTER not in text and time.time() < deadline:
            pump(0.2)
            if client.poll() is not None:
                fail(f"et exited after the wall: {text[-800:]!r}")
        if AFTER not in text:
            fail(f"control mode did not answer after the wall: {text[-800:]!r}")
        if client.poll() is not None:
            fail(f"et exited rc={client.returncode}")
        print("PASS: emerg wall stayed out of the tmux -CC stream", flush=True)
    finally:
        if master_fd >= 0:
            try:
                os.close(master_fd)
            except OSError:
                pass
        if client and client.poll() is None:
            client.send_signal(signal.SIGTERM)
            try:
                client.wait(timeout=2)
            except subprocess.TimeoutExpired:
                client.kill()
        subprocess.run(
            [str(tmux), "-L", socket_name, "kill-server"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if server and server.poll() is None:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=2)
            except subprocess.TimeoutExpired:
                server.kill()
        sshd.stop()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
