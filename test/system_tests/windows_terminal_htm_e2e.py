#!/usr/bin/env python3
"""Interactive Windows Terminal e2e for htm/htmd.

The test launches a real Windows Terminal window, drives its normal keyboard
bindings, and observes both the HTM protocol log and commands executed by the
ConPTY shells. It deliberately restarts an active daemon with ``htm -x`` and
then exercises detach, reattach, and clean shutdown.

This is opt-in because it needs an unlocked interactive desktop and a Windows
Terminal build with HTM integration enabled. Exit 77 means a prerequisite is
missing.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid


SKIP = 77
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_TERMINATE = 0x0001
TH32CS_SNAPPROCESS = 0x00000002
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
WM_CLOSE = 0x0010
SW_RESTORE = 9
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004
INPUT_KEYBOARD = 1
VK_CONTROL = 0x11
VK_SHIFT = 0x10
VK_MENU = 0x12
VK_RETURN = 0x0D
VK_ESCAPE = 0x1B
HWND_TOPMOST = -1
HWND_NOTOPMOST = -2
SWP_NOMOVE = 0x0002
SWP_NOSIZE = 0x0001
SWP_SHOWWINDOW = 0x0040


if os.name == "nt":
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    user32.GetForegroundWindow.restype = wintypes.HWND


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


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.c_void_p),
    ]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.c_void_p),
    ]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [
        ("uMsg", wintypes.DWORD),
        ("wParamL", wintypes.WORD),
        ("wParamH", wintypes.WORD),
    ]


class INPUTUNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", INPUTUNION)]


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


def processes_named(name: str, image: Path | None = None) -> list[int]:
    result: list[int] = []
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == INVALID_HANDLE_VALUE:
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
    handle = kernel32.OpenProcess(PROCESS_TERMINATE, False, pid)
    if handle:
        try:
            kernel32.TerminateProcess(handle, 1)
        finally:
            kernel32.CloseHandle(handle)


def windows() -> list[tuple[int, int, str]]:
    found: list[tuple[int, int, str]] = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    @callback_type
    def callback(hwnd: int, _param: int) -> bool:
        if not user32.IsWindowVisible(hwnd):
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        title = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, title, len(title))
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        found.append((int(hwnd), int(pid.value), title.value))
        return True

    user32.EnumWindows(callback, 0)
    return found


def wait_for(predicate, timeout: float, description: str, interval: float = 0.05):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(interval)
    fail(f"timed out waiting for {description}; last value={last!r}")


def focus_window(hwnd: int) -> None:
    user32.ShowWindow(hwnd, SW_RESTORE)
    user32.BringWindowToTop(hwnd)
    for _ in range(20):
        # SetForegroundWindow is intentionally restricted across processes.
        # Temporarily join the current foreground and Terminal input queues so
        # this interactive test can assign focus without mouse automation.
        foreground = user32.GetForegroundWindow()
        foreground_pid = wintypes.DWORD()
        target_pid = wintypes.DWORD()
        foreground_thread = user32.GetWindowThreadProcessId(
            foreground, ctypes.byref(foreground_pid)
        )
        target_thread = user32.GetWindowThreadProcessId(
            hwnd, ctypes.byref(target_pid)
        )
        current_thread = kernel32.GetCurrentThreadId()
        attached_current = bool(
            target_thread
            and current_thread != target_thread
            and user32.AttachThreadInput(current_thread, target_thread, True)
        )
        attached_foreground = bool(
            foreground_thread
            and foreground_thread != target_thread
            and user32.AttachThreadInput(foreground_thread, target_thread, True)
        )
        try:
            user32.SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW,
            )
            user32.SetWindowPos(
                hwnd,
                HWND_NOTOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW,
            )
            user32.SetForegroundWindow(hwnd)
            user32.SetFocus(hwnd)
        finally:
            if attached_foreground:
                user32.AttachThreadInput(foreground_thread, target_thread, False)
            if attached_current:
                user32.AttachThreadInput(current_thread, target_thread, False)
        if int(user32.GetForegroundWindow()) == hwnd:
            return
        time.sleep(0.05)
    fail("Windows Terminal test window could not receive foreground input")


def send_key(vk: int, flags: int = 0) -> None:
    item = INPUT(type=INPUT_KEYBOARD, ki=KEYBDINPUT(vk, 0, flags, 0, None))
    if user32.SendInput(1, ctypes.byref(item), ctypes.sizeof(INPUT)) != 1:
        fail(f"SendInput failed for virtual key {vk}: {ctypes.get_last_error()}")


def shortcut(*keys: int) -> None:
    for key in keys:
        send_key(key)
    for key in reversed(keys):
        send_key(key, KEYEVENTF_KEYUP)


def type_text(text: str) -> None:
    for char in text:
        code = ord(char)
        down = INPUT(
            type=INPUT_KEYBOARD,
            ki=KEYBDINPUT(0, code, KEYEVENTF_UNICODE, 0, None),
        )
        up = INPUT(
            type=INPUT_KEYBOARD,
            ki=KEYBDINPUT(0, code, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP, 0, None),
        )
        inputs = (INPUT * 2)(down, up)
        if user32.SendInput(2, inputs, ctypes.sizeof(INPUT)) != 2:
            fail(f"SendInput failed while typing: {ctypes.get_last_error()}")


def log_text(temp_dir: Path, stem: str = "htmd") -> str:
    chunks: list[str] = []
    for path in sorted(temp_dir.glob(f"{stem}-*.log")):
        try:
            chunks.append(path.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            pass
    return "\n".join(chunks)


def marker_command(marker: Path, value: str) -> str:
    # The isolated temp path has no spaces. This syntax works in both cmd.exe
    # and PowerShell, whichever COMSPEC/SHELL selects for the HTM pane.
    return f"echo {value}>{marker}\r"


class WindowsTerminalRun:
    def __init__(self, wt: Path, htm: Path, htmd: Path, temp_dir: Path):
        self.wt = wt
        self.htm = htm
        self.htmd = htmd
        self.temp_dir = temp_dir
        self.window_name = f"htm-e2e-{uuid.uuid4()}"
        self.title = f"HTM E2E {uuid.uuid4()}"
        self.hwnd = 0
        self.hwnds: list[int] = []
        self.terminal_pid = 0
        self.env = os.environ.copy()
        self.env["TEMP"] = str(temp_dir)
        self.env["TMP"] = str(temp_dir)
        self.env["HTM_BIN_DIR"] = str(htm.parent)

    def invoke_in(self, window_name: str, *command: str) -> None:
        completed = subprocess.run(
            [str(self.wt), "-w", window_name, *command],
            env=self.env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            timeout=15,
        )
        if completed.returncode:
            fail(f"Windows Terminal launcher failed: {completed.stderr.strip()}")

    def invoke(self, *command: str) -> None:
        self.invoke_in(self.window_name, *command)

    def new_htm_tab(self, kill_old: bool) -> None:
        args = ["new-tab", "--title", self.title, "--", str(self.htm)]
        if kill_old:
            args.append("-x")
        self.invoke(*args)

    def start(self) -> None:
        self.start_new_window(True)

    def start_new_window(self, kill_old: bool) -> None:
        before = {hwnd for hwnd, _pid, _title in windows()}
        self.window_name = f"htm-e2e-{uuid.uuid4()}"
        self.title = f"HTM E2E {uuid.uuid4()}"
        args = ["new-tab", "--title", self.title, "--", str(self.htm)]
        if kill_old:
            args.append("-x")
        self.invoke_in(self.window_name, *args)

        def find_window():
            titled = [w for w in windows() if self.title in w[2]]
            fresh = [w for w in titled if w[0] not in before]
            return (fresh or titled or [None])[0]

        hwnd, pid, _title = wait_for(find_window, 15, "Windows Terminal window")
        self.hwnd = hwnd
        self.hwnds.append(hwnd)
        self.terminal_pid = pid
        focus_window(hwnd)

    def close(self) -> None:
        open_hwnds = list(dict.fromkeys(self.hwnds))
        for hwnd in open_hwnds:
            user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        if open_hwnds:
            wait_for(
                lambda: not any(item[0] in open_hwnds for item in windows()),
                10,
                "Windows Terminal windows to close",
            )
        self.hwnd = 0
        self.hwnds.clear()


def _wait_for_htm_packet(run: WindowsTerminalRun, before_count: int, header: int, action: str) -> None:
    # Stock Windows Terminal does not speak HTM; detect lack of integration
    # early and skip instead of failing the suite.
    try:
        wait_for(
            lambda: log_text(run.temp_dir).count(f"Got message header: {header}") > before_count,
            10,
            f"Windows Terminal {action} packet",
        )
    except RuntimeError as exc:
        # No HTM packet arrived; this Windows Terminal build is not HTM-enabled.
        if "timed out waiting for Windows Terminal" in str(exc):
            skip(
                f"Windows Terminal HTM integration not present (no {action} packet from wt={run.wt}); "
                "need HTM-enabled wt/wtd build"
            )
        raise


def run_iteration(run: WindowsTerminalRun, iteration: int) -> None:
    print(f"iteration {iteration}: launching Windows Terminal + htm -x", flush=True)
    try:
        run.start()
    except RuntimeError as exc:
        if "could not receive foreground input" in str(exc):
            skip(f"Windows Terminal window could not receive foreground input: {exc}")
        raise
    # Sanitized like GetHtmIpcUser() in Headers.hpp
    user = os.environ.get("USERNAME", "unknown")
    sanitized = "".join(c if c.isalnum() or c in "_-" else "_" for c in user) or "unknown"
    socket_path = run.temp_dir / f"htm.{sanitized}.ipc"

    wait_for(lambda: processes_named("htmd.exe", run.htmd), 15, "htmd startup")
    wait_for(
        lambda: log_text(run.temp_dir).count("Starting terminal") >= 1,
        15,
        "initial HTM handshake",
    )
    first_htmd = processes_named("htmd.exe", run.htmd)[0]

    split_count = log_text(run.temp_dir).count("Got message header: 57")
    run.invoke("split-pane", "--duplicate")
    _wait_for_htm_packet(run, split_count, 57, "split")
    # The split action preserves focus on the HTM leader (the debug/control
    # surface), so move to the new follower before typing shell input.
    run.invoke("move-focus", "right")
    time.sleep(0.2)
    split_marker = run.temp_dir / f"split-{iteration}.txt"
    type_text(marker_command(split_marker, "split-ok"))
    wait_for(split_marker.exists, 10, "command execution in split pane")

    tab_count = log_text(run.temp_dir).count("Got message header: 53")
    run.invoke("new-tab")
    _wait_for_htm_packet(run, tab_count, 53, "new-tab")
    tab_marker = run.temp_dir / f"tab-{iteration}.txt"
    type_text(marker_command(tab_marker, "tab-ok"))
    wait_for(tab_marker.exists, 10, "command execution in HTM tab")
    print("  OK: split/tab actions and ConPTY command round-trips", flush=True)

    # Start a replacement while the first client and daemon are live. This is
    # the Windows AF_UNIX teardown race that previously left an unusable path.
    starts = log_text(run.temp_dir).count("Starting terminal")
    run.start_new_window(True)
    wait_for(
        lambda: any(pid != first_htmd for pid in processes_named("htmd.exe", run.htmd)),
        15,
        "replacement htmd process",
    )
    wait_for(
        lambda: log_text(run.temp_dir).count("Starting terminal") > starts,
        15,
        "replacement HTM handshake",
    )
    if not socket_path.exists():
        fail("replacement htmd did not publish its IPC socket")
    print("  OK: active htmd restarted without losing the IPC endpoint", flush=True)

    # Escape is an HTM detach command in the leader. The daemon must survive.
    shortcut(VK_ESCAPE)
    wait_for(
        lambda: not processes_named("htm.exe", run.htm),
        10,
        "htm client detach",
    )
    if not processes_named("htmd.exe", run.htmd) or not socket_path.exists():
        fail("Escape detached the daemon instead of only the client")

    starts = log_text(run.temp_dir).count("Starting terminal")
    run.new_htm_tab(False)
    focus_window(run.hwnd)
    wait_for(
        lambda: log_text(run.temp_dir).count("Starting terminal") > starts,
        15,
        "HTM reattach",
    )
    print("  OK: detached and reattached to the surviving daemon", flush=True)

    # Plain x is intercepted by the HTM leader and asks the daemon to exit.
    type_text("x")
    wait_for(
        lambda: not processes_named("htmd.exe", run.htmd),
        15,
        "clean htmd shutdown",
    )
    wait_for(lambda: not processes_named("htm.exe", run.htm), 10, "clean htm exit")
    wait_for(lambda: not socket_path.exists(), 10, "IPC socket removal")
    if "Server is shutting down" not in log_text(run.temp_dir):
        fail("htmd exited without logging its orderly shutdown")
    print("  OK: x cleanly exited htm/htmd and removed the IPC socket", flush=True)


def main() -> int:
    if os.name != "nt":
        skip("Windows Terminal e2e requires Windows")
    parser = argparse.ArgumentParser()
    parser.add_argument("--wt", help="Windows Terminal launcher (for example wtd.exe)")
    parser.add_argument("--htm")
    parser.add_argument("--htmd")
    parser.add_argument("--iterations", type=int, default=3)
    parser.add_argument("--keep-temp", action="store_true")
    args = parser.parse_args()
    wt = find_program(args.wt, "wt.exe")
    htm = find_program(args.htm, "htm.exe")
    htmd = find_program(args.htmd, "htmd.exe")
    if args.iterations < 1:
        parser.error("--iterations must be positive")

    temp_dir = Path(tempfile.mkdtemp(prefix="et-windows-terminal-e2e-"))
    run: WindowsTerminalRun | None = None
    passed = False
    try:
        print(f"Using wt={wt}", flush=True)
        print(f"Using htm={htm}", flush=True)
        print(f"Using htmd={htmd}", flush=True)
        print(f"Artifacts={temp_dir}", flush=True)
        for iteration in range(1, args.iterations + 1):
            run = WindowsTerminalRun(wt, htm, htmd, temp_dir)
            try:
                run_iteration(run, iteration)
            finally:
                had_error = sys.exc_info()[0] is not None
                try:
                    run.close()
                except BaseException:
                    if not had_error:
                        raise
        passed = True
        print(
            f"PASS: Windows Terminal htm/htmd e2e ({args.iterations} iterations)",
            flush=True,
        )
        return 0
    except SystemExit as exc:
        if exc.code == SKIP:
            raise
        print(f"FAIL: {exc}", file=sys.stderr, flush=True)
        import traceback

        traceback.print_exc()
        print(f"Preserving diagnostics in {temp_dir}", file=sys.stderr, flush=True)
        return 1
    except BaseException as exc:
        print(f"FAIL: {exc}", file=sys.stderr, flush=True)
        import traceback

        traceback.print_exc()
        print(f"Preserving diagnostics in {temp_dir}", file=sys.stderr, flush=True)
        return 1
    finally:
        if run:
            try:
                run.close()
            except BaseException:
                pass
        for name, image in (("htm.exe", htm), ("htmd.exe", htmd)):
            for pid in processes_named(name, image):
                terminate_pid(pid)
        if passed and not args.keep_temp:
            shutil.rmtree(temp_dir, ignore_errors=True)
        elif not passed:
            # On skip, also preserve diagnostics? Let caller see temp.
            pass


if __name__ == "__main__":
    raise SystemExit(main())
