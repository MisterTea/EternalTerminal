#!/usr/bin/env python3
"""PTY end-to-end tests for the real ``htm`` / ``htmd`` binaries.

Talks the HTM wire protocol over a PTY the same way iTerm2 does after
``ESC[###q``, covering rapid layout packets and clean daemon shutdown.
Does not require a GUI. Exit 77 if the binaries are missing.

``htm -x`` kills any existing ``htmd`` for this user.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import time
import uuid
from pathlib import Path
from typing import Optional

SKIP = 77
UUID_LEN = 36
INIT_SEQ = b"\x1b[###q"
EXIT_SEQ = b"\x1b[$$$q"

INSERT_KEYS = ord("1")
INIT_STATE = ord("2")
CLIENT_CLOSE_PANE = ord("3")
APPEND_TO_PANE = ord("4")
NEW_TAB = ord("5")
SERVER_CLOSE_PANE = ord("8")
NEW_SPLIT = ord("9")
RESIZE_PANE = ord("A")
DEBUG_LOG = ord("B")
INSERT_DEBUG_KEYS = ord("C")
SESSION_END = ord("D")


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


def pids_named(name: str) -> list[int]:
    try:
        out = subprocess.check_output(
            ["pgrep", "-x", "-U", str(uid()), name],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except subprocess.CalledProcessError:
        return []
    return [int(p) for p in out.split() if p.isdigit()]


def encode_length(length: int) -> bytes:
    return base64.b64encode(struct.pack("<i", length))


def encode_packet(header: int, payload: bytes = b"") -> bytes:
    if header == SESSION_END:
        return bytes([header])
    return bytes([header]) + encode_length(len(payload)) + payload


def new_id() -> str:
    return str(uuid.uuid4())


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


class HtmPty:
    def __init__(self, htm: Path, htmd: Path):
        self.htm = htm
        self.htmd = htmd
        self.master_fd = -1
        self.proc: Optional[subprocess.Popen] = None
        self.buf = bytearray()
        self.packets: list[tuple[int, bytes]] = []
        self.saw_init_seq = False
        self.init_json: Optional[dict] = None

    def start(self) -> None:
        env = os.environ.copy()
        env["PATH"] = f"{self.htm.parent.resolve()}:{env.get('PATH', '')}"
        self.master_fd, slave_fd = pty.openpty()
        self.proc = subprocess.Popen(
            [str(self.htm), "-x"],
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            env=env,
            close_fds=True,
            start_new_session=True,
        )
        os.close(slave_fd)
        deadline = time.time() + 15
        while time.time() < deadline:
            self.pump(0.2)
            if self.init_json is not None:
                self.drain_idle()
                return
            if self.proc.poll() is not None:
                fail(f"htm exited early with {self.proc.returncode}")
        fail("did not receive INIT_STATE from htm")

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
            self.buf.extend(chunk)
            self._parse()

    def _parse(self) -> None:
        if not self.saw_init_seq:
            idx = self.buf.find(INIT_SEQ)
            if idx < 0:
                return
            del self.buf[: idx + len(INIT_SEQ)]
            self.saw_init_seq = True
        while self.buf:
            header = self.buf[0]
            if header == SESSION_END:
                self.packets.append((header, b""))
                del self.buf[0]
                continue
            if len(self.buf) < 9:
                return
            try:
                length = struct.unpack("<i", base64.b64decode(bytes(self.buf[1:9])))[0]
            except Exception:
                del self.buf[0]
                continue
            if length < 0:
                del self.buf[0]
                continue
            if len(self.buf) < 9 + length:
                return
            payload = bytes(self.buf[9 : 9 + length])
            del self.buf[: 9 + length]
            self.packets.append((header, payload))
            if header == INIT_STATE and self.init_json is None:
                self.init_json = json.loads(payload.decode("utf-8"))

    def drain_idle(self, idle: float = 0.25, timeout: float = 5.0) -> None:
        deadline = time.time() + timeout
        last_change = time.time()
        prev = (len(self.buf), len(self.packets))
        while time.time() < deadline:
            self.pump(0.05)
            now = (len(self.buf), len(self.packets))
            if now != prev:
                last_change = time.time()
                prev = now
            elif time.time() - last_change >= idle:
                return

    def write_packet(self, header: int, payload: bytes = b"") -> None:
        if self.master_fd < 0:
            return
        if self.proc is not None and self.proc.poll() is not None:
            return
        try:
            os.write(self.master_fd, encode_packet(header, payload))
        except OSError:
            return

    def first_pane_id(self) -> str:
        if not self.init_json or not self.init_json.get("panes"):
            fail("INIT_STATE had no panes")
        return next(iter(self.init_json["panes"].keys()))

    def wait_header(self, header: int, timeout: float = 8.0) -> bytes:
        deadline = time.time() + timeout
        seen = 0
        initial = sum(1 for h, _ in self.packets if h == header)
        while time.time() < deadline:
            self.pump(0.15)
            now = [p for h, p in self.packets if h == header]
            if len(now) > initial:
                return now[-1]
            if self.proc and self.proc.poll() is not None and time.time() > deadline - timeout + 0.5:
                break
            seen = len(now)
        fail(f"timed out waiting for header {chr(header)!r} (saw {seen})")

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


def run_tests(htm: Path, htmd: Path) -> None:
    session = HtmPty(htm, htmd)
    try:
        session.start()
        if not session.saw_init_seq:
            fail("missing ESC[###q init sequence")
        pane = session.first_pane_id()
        if len(pane) != UUID_LEN:
            fail(f"pane id length {len(pane)} != {UUID_LEN}")
        if not pids_named("htmd"):
            fail("htmd did not start")
        if not ipc_path().exists():
            fail(f"missing IPC socket {ipc_path()}")
        print("OK: htm attached, INIT_STATE received, htmd listening", flush=True)

        marker = f"PTY{int(time.time())}"
        session.write_packet(
            INSERT_KEYS, pane.encode("ascii") + base64.b64encode(marker.encode())
        )
        session.pump(1.0)
        print("OK: INSERT_KEYS accepted", flush=True)

        session.write_packet(
            NEW_SPLIT, pane.encode("ascii") + new_id().encode("ascii") + b"1"
        )
        session.write_packet(NEW_TAB, new_id().encode("ascii") + new_id().encode("ascii"))
        session.pump(1.0)
        print("OK: NEW_SPLIT and NEW_TAB sent", flush=True)

        # Race: burst layout packets while the daemon is applying them.
        for i in range(12):
            session.write_packet(
                NEW_SPLIT, pane.encode("ascii") + new_id().encode("ascii") + b"0"
            )
            session.write_packet(
                NEW_TAB, new_id().encode("ascii") + new_id().encode("ascii")
            )
            if i % 3 == 2:
                session.pump(0.05)
        session.drain_idle()
        if not pids_named("htmd"):
            fail("htmd died during rapid NEW_SPLIT/NEW_TAB")
        if session.proc.poll() is not None:
            print(
                "OK: htm client dropped under flood; htmd stayed up",
                flush=True,
            )
        else:
            print("OK: rapid layout packets did not kill htm/htmd", flush=True)
            # Esc disconnects the client endpoint; daemon stays up.
            session.write_packet(INSERT_DEBUG_KEYS, b"\x1b")
            deadline = time.time() + 6
            while time.time() < deadline:
                session.pump(0.2)
                if any(h == SESSION_END for h, _ in session.packets) or EXIT_SEQ in bytes(
                    session.buf
                ):
                    break
            if not pids_named("htmd"):
                fail("htmd exited on Esc; expected detach, not shutdown")
            if not ipc_path().exists():
                fail("IPC socket vanished on Esc detach")
            print("OK: Esc detached without shutting down htmd", flush=True)
        session.stop(kill_htmd=False)
        if not pids_named("htmd"):
            fail("htmd exited when the htm client was stopped after Esc")
    except BaseException:
        session.stop()
        raise

    # Fresh attach, then shut the daemon down with gateway 'x'.
    session = HtmPty(htm, htmd)
    try:
        session.start()
        session.write_packet(INSERT_DEBUG_KEYS, b"x")
        start = time.time()
        deadline = start + 12
        resent = False
        while time.time() < deadline:
            session.pump(0.2)
            if not pids_named("htmd"):
                break
            if (
                not resent
                and session.proc is not None
                and session.proc.poll() is None
                and time.time() - start > 2
            ):
                session.write_packet(INSERT_DEBUG_KEYS, b"x")
                resent = True
        leftover = pids_named("htmd")
        if leftover:
            fail(f"htmd still running after INSERT_DEBUG_KEYS x: {leftover}")
        deadline = time.time() + 4
        while ipc_path().exists() and time.time() < deadline:
            time.sleep(0.1)
        if ipc_path().exists():
            fail("IPC socket still present after htmd shutdown")
        if session.proc.poll() is None:
            session.proc.wait(timeout=5)
        print("OK: x shut down htmd, removed IPC, htm exited", flush=True)
    finally:
        session.stop()

    if pids_named("htmd"):
        fail("htmd leftover after tests")
    if pids_named("htm"):
        fail("htm leftover after tests")
    print("PASS: htm/htmd PTY e2e", flush=True)


def main() -> int:
    if os.name == "nt":
        skip("PTY e2e requires a Unix PTY")
    parser = argparse.ArgumentParser()
    parser.add_argument("--htm")
    parser.add_argument("--htmd")
    args = parser.parse_args()
    htm = find_bin(args.htm, "HTM_BIN", "htm")
    htmd = find_bin(args.htmd, "HTMD_BIN", "htmd")
    print(f"Using htm={htm}", flush=True)
    print(f"Using htmd={htmd}", flush=True)
    run_tests(htm, htmd)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
