#!/usr/bin/env python3
"""PTY e2e for ``htm`` / ``htmd`` speaking tmux control mode (tmux -CC).

Drive the control protocol over a PTY: DCS 1000p, %session-changed, commands,
%output, detach, reconnect, kill-server. Exit 77 if binaries are missing.
"""

from __future__ import annotations

import argparse
import fcntl
import os
import pty
import re
import select
import shlex
import signal
import subprocess
import sys
import termios
import time
from pathlib import Path
from typing import Optional

SKIP = 77
DCS = b"\x1bP1000p"
ST = b"\x1b\\"


def skip(reason: str) -> None:
    print(f"SKIP: {reason}", flush=True)
    raise SystemExit(SKIP)


def fail(reason: str) -> None:
    print(f"FAIL: {reason}", flush=True)
    raise SystemExit(1)


def uid() -> int:
    return os.getuid()


def ipc_path() -> Path:
    return Path("/tmp") / f"htm.{uid()}.ipc"


def pids_named_from_proc(name: str) -> list[int]:
    proc = Path("/proc")
    if not proc.is_dir():
        return []
    my_uid = uid()
    pids: list[int] = []
    try:
        entries = list(proc.iterdir())
    except OSError:
        return []
    for entry in entries:
        if not entry.name.isdigit():
            continue
        try:
            comm = (entry / "comm").read_text(encoding="utf-8").strip()
        except OSError:
            continue
        if comm != name:
            continue
        try:
            status = (entry / "status").read_text(encoding="utf-8")
        except OSError:
            continue
        is_zombie = False
        uid_matches = False
        for line in status.splitlines():
            if line.startswith("State:"):
                fields = line.split()
                is_zombie = len(fields) > 1 and fields[1] == "Z"
            if line.startswith("Uid:"):
                uid_matches = int(line.split()[1]) == my_uid
        if uid_matches and not is_zombie:
            pids.append(int(entry.name))
    return pids


def pid_is_live(pid: int) -> bool:
    status_file = Path(f"/proc/{pid}/status")
    if status_file.is_file():
        try:
            for line in status_file.read_text(encoding="utf-8").splitlines():
                if line.startswith("State:"):
                    fields = line.split()
                    return len(fields) > 1 and fields[1] != "Z"
        except OSError:
            return False
    try:
        state = subprocess.check_output(
            ["ps", "-o", "stat=", "-p", str(pid)],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (FileNotFoundError, subprocess.CalledProcessError):
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False
    return bool(state) and not state.startswith("Z")


def pids_named(name: str) -> list[int]:
    proc = Path("/proc")
    if proc.is_dir():
        pids = pids_named_from_proc(name)
        if pids or not sys.platform.startswith("darwin"):
            return pids
    try:
        out = subprocess.check_output(
            ["pgrep", "-x", "-U", str(uid()), name],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except FileNotFoundError:
        return pids_named_from_proc(name)
    except subprocess.CalledProcessError:
        return pids_named_from_proc(name)
    return [int(p) for p in out.split() if p.isdigit() and pid_is_live(int(p))]


def find_bin(cli: Optional[str], env_key: str, name: str) -> Path:
    if cli and Path(cli).is_file():
        return Path(cli).resolve()
    env = os.environ.get(env_key)
    if env and Path(env).is_file():
        return Path(env).resolve()
    for candidate in (
        Path(__file__).resolve().parents[2] / "build" / name,
        Path.cwd() / name,
        Path.cwd() / "build" / name,
    ):
        if candidate.is_file():
            return candidate.resolve()
    skip(f"{name} binary is not built")


def octal_unescape(data: str) -> str:
    def repl(match: re.Match[str]) -> str:
        return chr(int(match.group(1), 8))

    return re.sub(r"\\([0-7]{3})", repl, data)


class HtmPty:
    def __init__(self, htm: Path, htmd: Path):
        self.htm = htm
        self.htmd = htmd
        self.master_fd = -1
        self.proc: Optional[subprocess.Popen] = None
        self.text = ""
        self.lines: list[str] = []

    def start(self, extra_args: list[str] | None = None) -> None:
        env = os.environ.copy()
        env["PATH"] = f"{self.htm.parent.resolve()}:{env.get('PATH', '')}"
        env.setdefault("SHELL", "/bin/sh")
        args = [str(self.htm)]
        if extra_args is None:
            args.append("-x")
        else:
            args.extend(extra_args)
        self.text = ""
        self.lines = []
        self.master_fd, slave_fd = pty.openpty()

        def _setup_slave() -> None:
            os.setsid()
            try:
                fcntl.ioctl(slave_fd, termios.TIOCSCTTY, 0)
            except OSError:
                pass

        self.proc = subprocess.Popen(
            args,
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            env=env,
            close_fds=True,
            preexec_fn=_setup_slave,
        )
        os.close(slave_fd)
        self.wait_until(
            lambda: "%session-changed" in self.text or DCS in self.text.encode(
                "latin1", "replace"
            ),
            15.0,
            "DCS / %session-changed",
        )
        self.wait_until(lambda: "%session-changed" in self.text, 10.0, "%session-changed")

    def pump(self, timeout: float) -> None:
        if self.master_fd < 0:
            return
        end = time.time() + timeout
        while time.time() < end:
            remaining = max(0.0, end - time.time())
            ready, _, _ = select.select([self.master_fd], [], [], remaining)
            if not ready:
                break
            try:
                chunk = os.read(self.master_fd, 65536)
            except OSError:
                break
            if not chunk:
                break
            self.text += chunk.decode("utf-8", "replace")
            self.lines = self.text.splitlines()

    def drain_idle(self, idle: float = 0.25, timeout: float = 5.0) -> None:
        deadline = time.time() + timeout
        last = time.time()
        prev = len(self.text)
        while time.time() < deadline:
            self.pump(0.05)
            if len(self.text) != prev:
                last = time.time()
                prev = len(self.text)
            elif time.time() - last >= idle:
                return

    def send(self, line: str) -> None:
        data = (line + "\n").encode()
        view = memoryview(data)
        while view:
            try:
                n = os.write(self.master_fd, view)
            except BlockingIOError:
                select.select([], [self.master_fd], [], 0.2)
                continue
            except OSError:
                return
            if n <= 0:
                return
            view = view[n:]

    def wait_until(self, predicate, timeout: float, description: str) -> None:
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pump(0.1)
            if predicate():
                return
            if self.proc is not None and self.proc.poll() is not None:
                fail(
                    f"htm exited while waiting for {description} "
                    f"(rc={self.proc.returncode})"
                )
        fail(f"timed out waiting for {description}: {self.text[-400:]!r}")

    def pane_output(self, pane: str) -> str:
        out = []
        prefix = f"%output %{pane} "
        ext = f"%extended-output %{pane} "
        for line in self.lines:
            if line.startswith(prefix):
                out.append(octal_unescape(line[len(prefix) :]))
            elif line.startswith(ext):
                idx = line.find(" : ")
                if idx >= 0:
                    out.append(octal_unescape(line[idx + 3 :]))
        return "".join(out)

    def pane_ids(self) -> list[str]:
        ids = []
        for line in self.lines:
            if line.startswith("%output %"):
                rest = line[len("%output %") :]
                pane = rest.split(" ", 1)[0]
                if pane not in ids:
                    ids.append(pane)
        return ids

    def stop(self, kill_htmd: bool = True) -> None:
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.send_signal(signal.SIGTERM)
            except OSError:
                pass
            try:
                self.proc.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)
        if self.master_fd >= 0:
            try:
                os.close(self.master_fd)
            except OSError:
                pass
            self.master_fd = -1
        if kill_htmd:
            for pid in pids_named("htmd"):
                try:
                    os.kill(pid, signal.SIGTERM)
                except OSError:
                    pass


def kill_named(name: str) -> None:
    for pid in pids_named(name):
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass


def run_leftover_stdin_test(htm: Path, htmd: Path) -> None:
    """Control lines queued with detach must not reach ``exec $SHELL`` / cat.

    WezTerm writes ``kill-pane`` on the PTY while HTM is tearing down. tmux
    consumes those bytes inside DCS; without a stdin drain, ``htm; exec cat``
    would echo them after WRAPPER_STARTED.
    """
    kill_named("htm")
    kill_named("htmd")
    time.sleep(0.3)

    env = os.environ.copy()
    env["PATH"] = f"{htm.parent.resolve()}:{env.get('PATH', '')}"
    env.setdefault("SHELL", "/bin/sh")
    master_fd, slave_fd = pty.openpty()
    wrapper = (
        f"{shlex.quote(str(htm))} -x; "
        "printf '\\nWRAPPER_STARTED\\n'; "
        "exec cat"
    )

    def _setup_wrapper_slave() -> None:
        os.setsid()
        try:
            fcntl.ioctl(slave_fd, termios.TIOCSCTTY, 0)
        except OSError:
            pass

    proc = subprocess.Popen(
        ["/bin/sh", "-c", wrapper],
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        env=env,
        close_fds=True,
        preexec_fn=_setup_wrapper_slave,
    )
    os.close(slave_fd)
    text = ""
    try:

        def pump(timeout: float) -> None:
            nonlocal text
            end = time.time() + timeout
            while time.time() < end:
                remaining = max(0.0, end - time.time())
                ready, _, _ = select.select([master_fd], [], [], remaining)
                if not ready:
                    break
                try:
                    chunk = os.read(master_fd, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                text += chunk.decode("utf-8", "replace")

        deadline = time.time() + 15
        while time.time() < deadline:
            pump(0.1)
            if "%session-changed" in text or "1000p" in text:
                break
            if proc.poll() is not None:
                fail(f"wrapper exited before attach (rc={proc.returncode}): {text[-400:]!r}")
        else:
            fail(f"timed out waiting for DCS: {text[-400:]!r}")

        os.write(master_fd, b"detach-client\nkill-pane -t %1\n")
        deadline = time.time() + 10
        while time.time() < deadline:
            pump(0.1)
            if "WRAPPER_STARTED" in text:
                break
        else:
            fail(f"wrapper did not resume after detach: {text[-800:]!r}")

        if "Killed: 9" in text:
            fail(f"htmd SIGKILL'd htm before a clean detach exit: {text[-800:]!r}")

        after = text.split("WRAPPER_STARTED", 1)[1]
        if "kill-pane" in after or "command not found" in after:
            fail(
                "detach leftover input reached the wrapping process: "
                f"{after[:500]!r}"
            )

        os.write(master_fd, b"POST_WRAPPER_MARK\n")
        deadline = time.time() + 5
        while time.time() < deadline:
            pump(0.1)
            if "POST_WRAPPER_MARK" in text:
                break
        else:
            fail(
                f"wrapping cat did not echo POST_WRAPPER_MARK rc={proc.poll()} "
                f"tail={text[-400:]!r}"
            )
        after = text.split("WRAPPER_STARTED", 1)[1]
        if "kill-pane" in after or "command not found" in after:
            fail(
                "detach leftover input reached the wrapping process: "
                f"{after[:500]!r}"
            )
        print("OK: detach leftover kill-pane did not reach wrapping shell", flush=True)
    finally:
        if proc.poll() is None:
            try:
                proc.send_signal(signal.SIGTERM)
            except OSError:
                pass
            try:
                proc.wait(timeout=4)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)
        try:
            os.close(master_fd)
        except OSError:
            pass
        kill_named("htm")
        kill_named("htmd")


def run_tests(htm: Path, htmd: Path) -> None:
    kill_named("htm")
    kill_named("htmd")
    time.sleep(0.3)
    session = HtmPty(htm, htmd)
    try:
        session.start()
        if DCS not in session.text.encode("latin1", "replace") and "1000p" not in session.text:
            fail("missing DCS 1000p")
        if "%session-changed" not in session.text:
            fail("missing %session-changed")
        if not pids_named("htmd"):
            fail("htmd did not start")
        print("OK: attached in control mode", flush=True)

        session.send("display-message -p '#{version}'")
        session.wait_until(lambda: "3.5a" in session.text, 8.0, "version")
        session.send("new-window")
        session.wait_until(lambda: "%window-add" in session.text, 8.0, "%window-add")
        session.send("split-window -h")
        session.wait_until(
            lambda: "%layout-change" in session.text, 8.0, "%layout-change"
        )
        session.send("send-keys -t %0 printf Space PTYMARK Enter")
        session.wait_until(
            lambda: "PTYMARK" in session.pane_output("0") or "PTYMARK" in session.text,
            10.0,
            "PTYMARK output",
        )
        print("OK: commands and %output", flush=True)

        session.send("")
        session.wait_until(lambda: "%exit" in session.text, 8.0, "%exit")
        session.stop(kill_htmd=False)
        if not pids_named("htmd"):
            fail("htmd exited on detach")
        print("OK: detach left htmd running", flush=True)
    except BaseException:
        session.stop()
        raise

    session = HtmPty(htm, htmd)
    try:
        session.start(extra_args=[])
        session.send("kill-server")
        deadline = time.time() + 12
        while time.time() < deadline and pids_named("htmd"):
            session.pump(0.2)
        if pids_named("htmd"):
            fail("htmd still running after kill-server")
        print("OK: kill-server", flush=True)
    finally:
        session.stop()

    run_leftover_stdin_test(htm, htmd)
    print("PASS: htm control-mode pty", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--htm")
    parser.add_argument("--htmd")
    args = parser.parse_args()
    htm = find_bin(args.htm, "HTM_BIN", "htm")
    htmd = find_bin(args.htmd, "HTMD_BIN", "htmd")
    run_tests(htm, htmd)


if __name__ == "__main__":
    main()
