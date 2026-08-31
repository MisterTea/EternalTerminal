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
        self.read_size = 65536

    def start(self) -> None:
        env = os.environ.copy()
        env["PATH"] = f"{self.htm.parent.resolve()}:{env.get('PATH', '')}"
        env.setdefault("SHELL", "/bin/sh")
        last_rc: Optional[int] = None
        leftover = b""
        for _attempt in range(3):
            self.buf.clear()
            self.packets.clear()
            self.saw_init_seq = False
            self.init_json = None
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
                    last_rc = self.proc.returncode
                    leftover = bytes(self.buf[-200:])
                    break
            if self.proc is not None and self.proc.poll() is None:
                self.proc.terminate()
                self.proc.wait(timeout=5)
            if self.master_fd >= 0:
                os.close(self.master_fd)
                self.master_fd = -1
        fail(
            f"htm exited early with {last_rc}; leftover={leftover!r}"
            if last_rc is not None
            else "did not receive INIT_STATE from htm"
        )

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
                chunk = os.read(self.master_fd, self.read_size)
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
        data = encode_packet(header, payload)
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

    def first_pane_id(self) -> str:
        if not self.init_json or not self.init_json.get("panes"):
            fail("INIT_STATE had no panes")
        return next(iter(self.init_json["panes"].keys()))

    def insert_keys(self, pane: str, data: str) -> None:
        self.write_packet(
            INSERT_KEYS, pane.encode("ascii") + base64.b64encode(data.encode())
        )

    def new_split(self, source: str, pane: str, vertical: bool = True) -> None:
        self.write_packet(
            NEW_SPLIT,
            source.encode("ascii") + pane.encode("ascii") + (b"1" if vertical else b"0"),
        )

    def new_tab(self, tab: str, pane: str) -> None:
        self.write_packet(NEW_TAB, tab.encode("ascii") + pane.encode("ascii"))

    def resize(self, pane: str, cols: int = 80, rows: int = 24) -> None:
        self.write_packet(
            RESIZE_PANE, encode_length(cols) + encode_length(rows) + pane.encode("ascii")
        )

    def pane_output(self, pane: str) -> str:
        chunks: list[str] = []
        for header, payload in self.packets:
            if header != APPEND_TO_PANE or len(payload) < UUID_LEN:
                continue
            if payload[:UUID_LEN] != pane.encode("ascii"):
                continue
            try:
                chunks.append(
                    base64.b64decode(payload[UUID_LEN:]).decode("utf-8", "replace")
                )
            except Exception:
                continue
        return "".join(chunks)

    def append_pane_order(self, start: int = 0) -> list[str]:
        order: list[str] = []
        for header, payload in self.packets[start:]:
            if header != APPEND_TO_PANE or len(payload) < UUID_LEN:
                continue
            order.append(payload[:UUID_LEN].decode("ascii", "replace"))
        return order

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
        fail(f"timed out waiting for {description}")

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

    def append_output(self) -> bytes:
        bodies = []
        for header, payload in self.packets:
            if header != APPEND_TO_PANE or len(payload) < UUID_LEN:
                continue
            encoded = payload[UUID_LEN:]
            if not encoded:
                continue
            try:
                bodies.append(base64.b64decode(encoded, validate=False))
            except Exception:
                continue
        return b"".join(bodies)

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
        pane = session.first_pane_id()
        tab_pane = new_id()
        session.write_packet(
            NEW_TAB, new_id().encode("ascii") + tab_pane.encode("ascii")
        )
        split_pane = new_id()
        session.write_packet(
            NEW_SPLIT, pane.encode("ascii") + split_pane.encode("ascii") + b"1"
        )
        session.pump(0.4)

        def start_printer(pane_id: str, tag: str) -> None:
            cmd = (
                f"i=1; while [ \"$i\" -le 24 ]; do printf '{tag}_%s\\n' \"$i\"; "
                "i=$((i+1)); done &\n"
            )
            session.write_packet(
                INSERT_KEYS, pane_id.encode("ascii") + base64.b64encode(cmd.encode())
            )

        start_printer(pane, "ST0")
        start_printer(tab_pane, "ST1")
        start_printer(split_pane, "ST2")
        for i in range(20):
            marker = f"printf 'PTYKEY_{i}\\n'\n"
            session.write_packet(
                INSERT_KEYS, pane.encode("ascii") + base64.b64encode(marker.encode())
            )
            session.pump(0.04)
        session.drain_idle(idle=0.3, timeout=10)
        if session.proc is not None and session.proc.poll() is not None:
            fail("htm died under concurrent pane output and INSERT_KEYS")
        if not pids_named("htmd"):
            fail("htmd died under concurrent pane output and INSERT_KEYS")
        out = session.append_output()
        if b"PTYKEY_19" not in out:
            fail("INSERT_KEYS stalled while panes were printing")
        if b"ST0_1" not in out or b"ST1_1" not in out or b"ST2_1" not in out:
            fail("missing concurrent pane output during INSERT_KEYS stress")
        print("OK: concurrent printers and INSERT_KEYS did not drop htm", flush=True)

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
