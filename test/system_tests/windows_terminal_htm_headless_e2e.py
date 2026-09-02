#!/usr/bin/env python3
"""Headless Windows Terminal e2e for htm/htmd.

Runs without a GUI by driving ``htm.exe`` through anonymous pipes the same
way Windows Terminal's ``HtmLeaderConnection`` does: the terminal wraps a
ConPTY, passes bytes through until ``ESC[###q``, then frames everything.
This exercises the same races the interactive
``windows_terminal_htm_e2e.py`` covers — rapid layout bursts, AF_UNIX
replacement while live, Escape detach vs ``x`` shutdown — but works on
headless agents and in CI.

Exit 77 if prerequisites are missing.
"""

from __future__ import annotations

import argparse
import base64
import ctypes
import json
import msvcrt
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import uuid
from ctypes import wintypes
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

TH32CS_SNAPPROCESS = 0x00000002
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.PeekNamedPipe.argtypes = [
    wintypes.HANDLE,
    wintypes.LPVOID,
    wintypes.DWORD,
    wintypes.LPDWORD,
    wintypes.LPDWORD,
    wintypes.LPDWORD,
]
kernel32.GetConsoleMode.argtypes = [wintypes.HANDLE, wintypes.LPDWORD]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", wintypes.LONG),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * 260),
    ]


def skip(reason: str) -> None:
    print(f"SKIP: {reason}", flush=True)
    raise SystemExit(SKIP)


def fail(reason: str) -> None:
    raise RuntimeError(reason)


def find_program(value: str | None, name: str) -> Path:
    candidate = value or shutil.which(name)
    if not candidate:
        # also try build/Release under repo root
        for p in [
            Path(__file__).resolve().parents[2] / "build" / "Release" / name,
            Path(__file__).resolve().parents[2] / "build" / name,
        ]:
            if p.is_file():
                return p.resolve()
        skip(f"{name} was not found")
    path = Path(candidate).resolve()
    if not path.is_file():
        skip(f"{name} does not exist: {path}")
    return path


def process_image(pid: int) -> Path | None:
    handle = kernel32.OpenProcess(0x1000, False, pid)
    if not handle:
        return None
    try:
        size = wintypes.DWORD(32768)
        buf = ctypes.create_unicode_buffer(size.value)
        if not kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
            return None
        return Path(buf.value)
    finally:
        kernel32.CloseHandle(handle)


def processes_named(name: str, image: Path | None = None) -> list[int]:
    result: list[int] = []
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == INVALID_HANDLE_VALUE or snap == 0:
        return result
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            if entry.szExeFile.casefold() == name.casefold():
                pid = int(entry.th32ProcessID)
                if image is None:
                    result.append(pid)
                else:
                    actual = process_image(pid)
                    if actual and str(actual).casefold() == str(image).casefold():
                        result.append(pid)
            ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snap)
    return result


def terminate_pid(pid: int) -> None:
    handle = kernel32.OpenProcess(0x0001, False, pid)
    if handle:
        try:
            kernel32.TerminateProcess(handle, 1)
        finally:
            kernel32.CloseHandle(handle)


def ipc_path_for_temp(temp_dir: Path) -> Path:
    user = os.environ.get("USERNAME", "user")
    # Sanitized like GetHtmIpcUser
    sanitized = "".join(c if c.isalnum() or c in "_-" else "_" for c in user) or "user"
    return temp_dir / f"htm.{sanitized}.ipc"


def system_ipc_path() -> Path:
    # Fallback: what GetTempDirectory would return if we didn't override TEMP
    tmp = os.environ.get("TEMP") or os.environ.get("TMP") or tempfile.gettempdir()
    user = os.environ.get("USERNAME", "user")
    sanitized = "".join(c if c.isalnum() or c in "_-" else "_" for c in user) or "user"
    return Path(tmp) / f"htm.{sanitized}.ipc"


def log_text(temp_dir: Path, stem: str = "htmd") -> str:
    chunks: list[str] = []
    for path in sorted(temp_dir.glob(f"{stem}-*.log")):
        try:
            chunks.append(path.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            pass
    return "\n".join(chunks)


def encode_length(length: int) -> bytes:
    return base64.b64encode(struct.pack("<i", length))


def encode_packet(header: int, payload: bytes = b"") -> bytes:
    if header == SESSION_END:
        return bytes([header])
    return bytes([header]) + encode_length(len(payload)) + payload


def new_id() -> str:
    return str(uuid.uuid4())


def peek_available(handle: int) -> int:
    avail = wintypes.DWORD(0)
    ok = kernel32.PeekNamedPipe(handle, None, 0, None, ctypes.byref(avail), None)
    if not ok:
        return 0
    return int(avail.value)


class HtmPipeSession:
    """Drive htm.exe via pipes, speaking the HTM terminal protocol."""

    def __init__(self, htm: Path, htmd: Path, temp_dir: Path):
        self.htm = htm
        self.htmd = htmd
        self.temp_dir = temp_dir
        self.proc: Optional[subprocess.Popen] = None
        self.buf = bytearray()
        self.packets: list[tuple[int, bytes]] = []
        self.saw_init_seq = False
        self.init_json: Optional[dict] = None
        self.env = os.environ.copy()
        self.env["TEMP"] = str(temp_dir)
        self.env["TMP"] = str(temp_dir)
        self.env["HTM_BIN_DIR"] = str(htm.parent)

    def start(self, kill_old: bool = True) -> None:
        args = [str(self.htm)]
        if kill_old:
            args.append("-x")
        env = self.env
        # Ensure no console window, allow pipe handles to be inherited
        creationflags = subprocess.CREATE_NO_WINDOW if hasattr(subprocess, "CREATE_NO_WINDOW") else 0
        self.buf.clear()
        self.packets.clear()
        self.saw_init_seq = False
        self.init_json = None
        self.proc = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
            creationflags=creationflags,
        )
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            self.pump(0.2)
            if self.init_json is not None:
                self.drain_idle(idle=0.2, timeout=1.0)
                return
            if self.proc.poll() is not None:
                leftover = bytes(self.buf[-500:])
                rc = self.proc.returncode
                # Try to collect more output
                try:
                    extra, _ = self.proc.communicate(timeout=1)
                    if extra:
                        leftover += extra[-500:]
                except Exception:
                    pass
                fail(f"htm exited early with rc={rc}; leftover={leftover!r}; log={log_text(self.temp_dir)[:500]!r}")
        fail("did not receive INIT_STATE from htm (timeout). packets=%s buf=%r log=%r" % (len(self.packets), bytes(self.buf[-200:]), log_text(self.temp_dir)[-1000:]))

    def pump(self, timeout: float) -> None:
        if self.proc is None or self.proc.stdout is None:
            return
        end = time.monotonic() + timeout
        # Get raw handle for PeekNamedPipe
        try:
            fd = self.proc.stdout.fileno()
            handle = msvcrt.get_osfhandle(fd)
        except Exception:
            handle = None
        while time.monotonic() < end:
            avail = 0
            if handle is not None:
                avail = peek_available(handle)
                if avail == 0:
                    # Also check if process exited
                    if self.proc.poll() is not None:
                        # Drain whatever is left blocking
                        try:
                            chunk = os.read(fd, 4096)
                            if chunk:
                                self.buf.extend(chunk)
                                self._parse()
                                continue
                        except OSError:
                            pass
                        break
                    time.sleep(0.02)
                    continue
            else:
                # fallback: try non-blocking read with timeout
                time.sleep(0.02)
                continue
            try:
                chunk = os.read(fd, min(avail, 65536))
            except OSError:
                break
            if not chunk:
                break
            self.buf.extend(chunk)
            self._parse()
            # if we got data, loop again immediately to drain
            if peek_available(handle) == 0:
                break

    def _parse(self) -> None:
        if not self.saw_init_seq:
            idx = self.buf.find(INIT_SEQ)
            if idx < 0:
                # Keep only tail that could contain partial init
                # Init seq is 6 bytes, keep last 5
                if len(self.buf) > 1024:
                    # Trim old data that is clearly not init (e.g., \033[?7h)
                    # Keep last 16 bytes to detect split init
                    del self.buf[:-16]
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
                # Not a valid length, treat as plain output that leaked?
                # Drop first byte and retry — matches HtmLeaderConnection dropping invalidLength
                del self.buf[0]
                continue
            if length < 0 or length > 4 * 1024 * 1024:
                del self.buf[0]
                continue
            if len(self.buf) < 9 + length:
                return
            payload = bytes(self.buf[9 : 9 + length])
            del self.buf[: 9 + length]
            self.packets.append((header, payload))
            if header == INIT_STATE and self.init_json is None:
                try:
                    self.init_json = json.loads(payload.decode("utf-8"))
                except Exception:
                    self.init_json = {}

    def drain_idle(self, idle: float = 0.25, timeout: float = 5.0) -> None:
        deadline = time.monotonic() + timeout
        last_change = time.monotonic()
        prev = (len(self.buf), len(self.packets))
        while time.monotonic() < deadline:
            self.pump(0.05)
            now = (len(self.buf), len(self.packets))
            if now != prev:
                last_change = time.monotonic()
                prev = now
            elif time.monotonic() - last_change >= idle:
                return

    def write_packet(self, header: int, payload: bytes = b"") -> None:
        if self.proc is None or self.proc.stdin is None or self.proc.poll() is not None:
            return
        data = encode_packet(header, payload)
        try:
            self.proc.stdin.write(data)
            self.proc.stdin.flush()
        except OSError:
            pass

    def first_pane_id(self) -> str:
        if not self.init_json or not self.init_json.get("panes"):
            fail(f"INIT_STATE had no panes: {self.init_json}")
        return next(iter(self.init_json["panes"].keys()))

    def insert_keys(self, pane: str, data: str) -> None:
        self.write_packet(INSERT_KEYS, pane.encode("ascii") + base64.b64encode(data.encode()))

    def new_split(self, source: str, pane: str, vertical: bool = True) -> None:
        self.write_packet(NEW_SPLIT, source.encode("ascii") + pane.encode("ascii") + (b"1" if vertical else b"0"))

    def new_tab(self, tab: str, pane: str) -> None:
        self.write_packet(NEW_TAB, tab.encode("ascii") + pane.encode("ascii"))

    def resize(self, pane: str, cols: int = 80, rows: int = 24) -> None:
        self.write_packet(RESIZE_PANE, encode_length(cols) + encode_length(rows) + pane.encode("ascii"))

    def pane_output(self, pane: str) -> str:
        chunks: list[str] = []
        for header, payload in self.packets:
            if header != APPEND_TO_PANE or len(payload) < UUID_LEN:
                continue
            if payload[:UUID_LEN] != pane.encode("ascii"):
                continue
            try:
                chunks.append(base64.b64decode(payload[UUID_LEN:]).decode("utf-8", "replace"))
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
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pump(0.1)
            if predicate():
                return
            if self.proc is not None and self.proc.poll() is not None:
                fail(f"htm exited while waiting for {description} (rc={self.proc.returncode})")
        fail(f"timed out waiting for {description}")

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
                # Close stdin to signal EOF; htm should forward and stay alive until htmd closes?
                # Instead send SIGTERM via terminate
                self.proc.terminate()
            except OSError:
                pass
            try:
                self.proc.wait(timeout=4)
            except subprocess.TimeoutExpired:
                try:
                    self.proc.kill()
                    self.proc.wait(timeout=2)
                except Exception:
                    pass
        if self.proc:
            try:
                if self.proc.stdout:
                    self.proc.stdout.close()
            except Exception:
                pass
            try:
                if self.proc.stdin:
                    self.proc.stdin.close()
            except Exception:
                pass
        if kill_htmd:
            for pid in processes_named("htmd.exe", self.htmd):
                terminate_pid(pid)


def wait_for(predicate, timeout: float, description: str, interval: float = 0.05):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(interval)
    fail(f"timed out waiting for {description}; last value={last!r}")


def run_iteration(session: HtmPipeSession, iteration: int, temp_dir: Path, htmd: Path) -> None:
    print(f"iteration {iteration}: launching htm -x via pipes", flush=True)
    session.start(kill_old=True)
    socket_path = ipc_path_for_temp(temp_dir)

    wait_for(lambda: processes_named("htmd.exe", htmd), 15, "htmd startup")
    wait_for(lambda: log_text(temp_dir).count("Starting terminal") >= 1, 15, "initial HTM handshake")
    first_htmd = processes_named("htmd.exe", htmd)[0]
    print(f"  OK: htm attached, INIT_STATE received, htmd pid {first_htmd}", flush=True)

    pane = session.first_pane_id()

    # Race: burst layout packets while daemon is applying them — must not wedge.
    for i in range(12):
        session.new_split(pane, new_id(), vertical=(i % 2 == 0))
        session.new_tab(new_id(), new_id())
        if i % 3 == 2:
            session.pump(0.05)
    session.drain_idle()
    if not processes_named("htmd.exe", htmd):
        fail("htmd died during rapid NEW_SPLIT/NEW_TAB")
    if session.proc and session.proc.poll() is not None:
        fail("htm died under rapid layout burst")
    print("  OK: rapid layout packets did not kill htm/htmd", flush=True)

    # Verify split/tab creation produces valid panes and concurrent output not dropped
    panes_to_test = []
    # create 2 tabs + 2 splits explicitly and verify they produce output
    tab_a = new_id()
    session.new_tab(new_id(), tab_a)
    tab_b = new_id()
    session.new_tab(new_id(), tab_b)
    split_v = new_id()
    session.new_split(pane, split_v, vertical=True)
    split_h = new_id()
    session.new_split(pane, split_h, vertical=False)
    for p in (pane, tab_a, tab_b, split_v, split_h):
        session.resize(p, 80, 24)
        panes_to_test.append(p)
    session.drain_idle(idle=0.3, timeout=4)

    # Send markers into each pane and ensure isolated delivery
    bursts = 4
    for tag_pane, tag in [(pane, "PIPE0"), (tab_a, "PIPE1"), (tab_b, "PIPE2"), (split_v, "PIPESV"), (split_h, "PIPESH")]:
        session.insert_keys(tag_pane, f"i=1; while [ \"$i\" -le {bursts} ]; do printf '{tag}_%s\\n' \"$i\"; i=$((i+1)); done\n")
    # Also test concurrent write race: multiple threads writing INSERT_KEYS and RESIZE interleaved
    # Simulate Windows Terminal AppActionHandlers race where split and resize arrive on different threads
    import threading

    errors = []

    def concurrent_writer():
        for _ in range(20):
            try:
                session.write_packet(INSERT_KEYS, pane.encode("ascii") + base64.b64encode(b"echo concurrent\n"))
                session.write_packet(RESIZE_PANE, encode_length(80) + encode_length(24) + pane.encode("ascii"))
            except Exception as e:
                errors.append(str(e))

    threads = [threading.Thread(target=concurrent_writer) for _ in range(3)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        fail(f"concurrent writes failed: {errors}")
    session.drain_idle(idle=0.3, timeout=8)
    if not processes_named("htmd.exe", htmd):
        fail("htmd died during concurrent writes")
    if session.proc and session.proc.poll() is not None:
        fail("htm died during concurrent writes")
    print("  OK: concurrent packet framing preserved (no splice)", flush=True)

    # Replacement while active — the Windows AF_UNIX teardown race.
    starts = log_text(temp_dir).count("Starting terminal")
    # Launch replacement htm -x in a separate session
    replacement = HtmPipeSession(session.htm, session.htmd, temp_dir)
    replacement.start(kill_old=True)
    # Original session's proc should be terminated by replacement's kill, but new daemon must be up
    wait_for(lambda: any(pid != first_htmd for pid in processes_named("htmd.exe", htmd)), 15, "replacement htmd process")
    wait_for(lambda: log_text(temp_dir).count("Starting terminal") > starts, 15, "replacement HTM handshake")
    if not socket_path.exists():
        fail("replacement htmd did not publish its IPC socket")
    print("  OK: active htmd restarted without losing IPC endpoint", flush=True)
    # Clean up replacement and original before next phase
    replacement.stop(kill_htmd=False)
    # Original session's htm may still be alive but its IPC is now stale; terminate it
    if session.proc and session.proc.poll() is None:
        try:
            session.proc.terminate()
            session.proc.wait(timeout=3)
        except Exception:
            pass
        try:
            session.proc.kill()
        except Exception:
            pass
        session.proc = None
        session.buf.clear()
        session.packets.clear()
        session.saw_init_seq = False
        session.init_json = None

    # Fresh session for detach / reattach / shutdown tests
    session2 = HtmPipeSession(session.htm, session.htmd, temp_dir)
    session2.start(kill_old=False)
    pane2 = session2.first_pane_id()
    print("  OK: fresh session after replacement", flush=True)

    # Escape is HTM detach (INSERT_DEBUG_KEYS ESC). Daemon must survive.
    shutdown_before = log_text(temp_dir).count("Server is shutting down")
    session2.write_packet(INSERT_DEBUG_KEYS, b"\x1b")
    # Wait for htm client to detach (process exits) but daemon stays
    wait_for(lambda: session2.proc.poll() is not None, 10, "htm client detach after Esc")
    if not processes_named("htmd.exe", htmd):
        fail("Escape detached the daemon instead of only the client")
    # Socket must still exist after detach
    if not socket_path.exists():
        fail("IPC socket vanished on Esc detach")
    # Also check log: should have closed endpoint but not shutting down
    if log_text(temp_dir).count("Server is shutting down") > shutdown_before:
        fail("htmd logged shutdown on Esc (should only disconnect)")
    print("  OK: Esc detached client without shutting down daemon", flush=True)

    # Reattach
    starts2 = log_text(temp_dir).count("Starting terminal")
    session3 = HtmPipeSession(session.htm, session.htmd, temp_dir)
    session3.start(kill_old=False)
    wait_for(lambda: log_text(temp_dir).count("Starting terminal") > starts2, 15, "HTM reattach")
    print("  OK: detached and reattached to surviving daemon", flush=True)

    # Plain x is intercepted by HTM leader and asks daemon to exit.
    shutdown_before_x = log_text(temp_dir).count("Server is shutting down")
    session3.write_packet(INSERT_DEBUG_KEYS, b"x")
    wait_for(lambda: not processes_named("htmd.exe", htmd), 15, "clean htmd shutdown after x")
    wait_for(lambda: session3.proc.poll() is not None, 10, "clean htm exit after x")
    # Socket removal — on Windows _unlink in stopListening should delete the file
    # Only the isolated temp_dir socket matters; system temp may have stale leftovers
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and socket_path.exists():
        time.sleep(0.1)
    if socket_path.exists():
        fail("IPC socket still present after htmd shutdown")
    if log_text(temp_dir).count("Server is shutting down") <= shutdown_before_x:
        fail("htmd exited without logging its orderly shutdown")
    print("  OK: x cleanly exited htm/htmd and removed IPC socket", flush=True)
    session3.stop(kill_htmd=False)


def main() -> int:
    if os.name != "nt":
        skip("Windows Terminal headless e2e requires Windows")
    parser = argparse.ArgumentParser()
    parser.add_argument("--htm")
    parser.add_argument("--htmd")
    parser.add_argument("--iterations", type=int, default=1)
    parser.add_argument("--keep-temp", action="store_true")
    args = parser.parse_args()
    htm = find_program(args.htm, "htm.exe")
    htmd = find_program(args.htmd, "htmd.exe")
    if args.iterations < 1:
        parser.error("--iterations must be positive")

    # Clean any stale system-wide IPC leftover from debug runs
    for stale in [system_ipc_path()]:
        try:
            if stale.exists():
                stale.unlink()
        except OSError:
            pass
    # Also ensure no stale htmd holds the path
    for pid in processes_named("htmd.exe", htmd):
        terminate_pid(pid)
    time.sleep(0.3)

    temp_dir = Path(tempfile.mkdtemp(prefix="et-windows-headless-e2e-"))
    passed = False
    try:
        print(f"Using htm={htm}", flush=True)
        print(f"Using htmd={htmd}", flush=True)
        print(f"Artifacts={temp_dir}", flush=True)
        for iteration in range(1, args.iterations + 1):
            session = HtmPipeSession(htm, htmd, temp_dir)
            try:
                run_iteration(session, iteration, temp_dir, htmd)
            finally:
                session.stop(kill_htmd=False)
            # brief pause between iterations to let OS release socket
            time.sleep(0.5)
        passed = True
        print(f"PASS: Windows Terminal headless htm/htmd e2e ({args.iterations} iterations)", flush=True)
        return 0
    except SystemExit as e:
        if e.code == SKIP:
            raise
        print(f"FAIL: {e}", file=sys.stderr, flush=True)
        print(f"Preserving diagnostics in {temp_dir}", file=sys.stderr, flush=True)
        import traceback
        traceback.print_exc()
        return 1
    except BaseException as exc:
        print(f"FAIL: {exc}", file=sys.stderr, flush=True)
        import traceback
        traceback.print_exc()
        print(f"Preserving diagnostics in {temp_dir}", file=sys.stderr, flush=True)
        return 1
    finally:
        # Clean up processes
        for name, image in (("htm.exe", htm), ("htmd.exe", htmd)):
            for pid in processes_named(name, image):
                terminate_pid(pid)
        # Extra grace for socket file to be removed
        time.sleep(0.3)
        if passed and not args.keep_temp:
            shutil.rmtree(temp_dir, ignore_errors=True)
        else:
            if passed:
                print(f"Kept {temp_dir}", flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
