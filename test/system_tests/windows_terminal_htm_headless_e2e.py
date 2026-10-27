#!/usr/bin/env python3
"""Headless Windows tmux-control-mode e2e for ``htm`` / ``htmd``.

This is the pipe equivalent of a terminal emulator's tmux -CC integration.
It verifies the Windows bridge without requiring an unlocked desktop; the
interactive Windows Terminal driver uses the same control-mode protocol.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import shutil
import subprocess
import time

SKIP = 77
DCS = b"\x1bP1000p"
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_TERMINATE = 0x0001
TH32CS_SNAPPROCESS = 0x00000002
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


def skip(reason: str) -> None:
    print(f"SKIP: {reason}", flush=True)
    raise SystemExit(SKIP)


def fail(reason: str) -> None:
    raise RuntimeError(reason)


def find_program(value: str | None, name: str) -> Path:
    candidate = value or shutil.which(name)
    if not candidate:
        skip(f"{name} was not found")
    path = Path(candidate).resolve()
    if not path.is_file():
        skip(f"{name} does not exist: {path}")
    return path


if os.name == "nt":
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.OpenProcess.restype = wintypes.HANDLE


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


def process_image(pid: int) -> Path | None:
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
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


def pids_named(name: str, image: Path) -> list[int]:
    result: list[int] = []
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap in (0, INVALID_HANDLE_VALUE):
        return result
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            if entry.szExeFile.casefold() == name.casefold():
                actual = process_image(int(entry.th32ProcessID))
                if actual and str(actual).casefold() == str(image).casefold():
                    result.append(int(entry.th32ProcessID))
            ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snap)
    return result


def terminate(pid: int) -> None:
    handle = kernel32.OpenProcess(PROCESS_TERMINATE, False, pid)
    if handle:
        try:
            kernel32.TerminateProcess(handle, 1)
        finally:
            kernel32.CloseHandle(handle)


class ControlPipe:
    def __init__(self, htm: Path, htmd: Path):
        self.htm = htm
        self.htmd = htmd
        self.proc: subprocess.Popen[bytes] | None = None
        self.raw = bytearray()
        self.text = ""

    def start(self, replace: bool) -> None:
        args = [str(self.htm)] + (["-x"] if replace else [])
        self.raw.clear()
        self.text = ""
        self.proc = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        self.wait_for(lambda: DCS in self.raw and "%session-changed" in self.text,
                      15, "DCS and control-mode snapshot")

    def pump(self, timeout: float = 0.1) -> None:
        if not self.proc or not self.proc.stdout:
            return
        import msvcrt

        handle = msvcrt.get_osfhandle(self.proc.stdout.fileno())
        available = wintypes.DWORD()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if not kernel32.PeekNamedPipe(handle, None, 0, None, ctypes.byref(available), None):
                return
            if not available.value:
                return
            chunk = os.read(self.proc.stdout.fileno(), min(available.value, 65536))
            if not chunk:
                return
            self.raw.extend(chunk)
            self.text = self.raw.decode("utf-8", "replace")

    def wait_for(self, predicate, timeout: float, description: str) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pump(0.1)
            if predicate():
                return
            if self.proc and self.proc.poll() is not None:
                fail(f"htm exited waiting for {description} (rc={self.proc.returncode}); "
                     f"output={self.raw[-600:]!r}")
            time.sleep(0.02)
        fail(f"timed out waiting for {description}; output={self.raw[-1000:]!r}")

    def send(self, command: str) -> None:
        if not self.proc or not self.proc.stdin:
            fail("htm stdin is unavailable")
        self.proc.stdin.write((command + "\r\n").encode())
        self.proc.stdin.flush()

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)


def run_iteration(htm: Path, htmd: Path, iteration: int) -> None:
    session = ControlPipe(htm, htmd)
    try:
        print(f"iteration {iteration}: attach through anonymous pipes", flush=True)
        session.start(replace=True)
        if not pids_named("htmd.exe", htmd):
            fail("htmd did not start")

        session.send("display-message -p '#{version}'")
        session.wait_for(lambda: "3.5a" in session.text, 8, "tmux version reply")
        session.send("refresh-client -C 100x30")
        session.send("new-window")
        session.wait_for(lambda: "%window-add" in session.text, 8, "new-window notification")
        session.send("split-window -h -t %0")
        session.wait_for(lambda: "%layout-change" in session.text, 8, "split layout notification")
        session.send("send-keys -t %0 echo WIN_PIPE_MARK Enter")
        session.wait_for(lambda: "WIN_PIPE_MARK" in session.text, 10, "pane output")
        print("  OK: tmux -CC attach, commands, layout, and pane output", flush=True)

        old = pids_named("htmd.exe", htmd)
        replacement = ControlPipe(htm, htmd)
        try:
            replacement.start(replace=True)
            replacement.wait_for(lambda: "%session-changed" in replacement.text,
                                 8, "replacement snapshot")
            if old and pids_named("htmd.exe", htmd) == old:
                fail("htm -x did not replace the active htmd")
            replacement.send("kill-server")
            replacement.wait_for(lambda: "%exit" in replacement.text, 10, "kill-server exit")
        finally:
            replacement.stop()
        print("  OK: active daemon replacement and orderly shutdown", flush=True)
    finally:
        session.stop()
        for pid in pids_named("htmd.exe", htmd):
            terminate(pid)


def main() -> int:
    if os.name != "nt":
        skip("Windows-only test")
    parser = argparse.ArgumentParser()
    parser.add_argument("--htm")
    parser.add_argument("--htmd")
    parser.add_argument("--iterations", type=int, default=1)
    args = parser.parse_args()
    htm = find_program(args.htm, "htm.exe")
    htmd = find_program(args.htmd, "htmd.exe")
    if args.iterations < 1:
        parser.error("--iterations must be positive")
    for iteration in range(1, args.iterations + 1):
        run_iteration(htm, htmd, iteration)
    print(f"PASS: Windows headless HTM control-mode e2e ({args.iterations} iterations)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
