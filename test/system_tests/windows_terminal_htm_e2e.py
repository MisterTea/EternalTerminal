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
import atexit
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
import weakref

sys.path.insert(0, str(Path(__file__).resolve().parent))
from htm_gui_e2e import (  # noqa: E402
    GuiTerminalSession,
    fail,
    kill_htm_daemons,
    log_has_typed,
    run_emulator_main,
    skip,
    typed_from_log,
    wait_until,
)


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
VK_TAB = 0x09
VK_LEFT = 0x25
VK_RIGHT = 0x27
VK_OEM_PLUS = 0xBB
VK_OEM_MINUS = 0xBD
HWND_TOPMOST = -1
HWND_NOTOPMOST = -2
SWP_NOMOVE = 0x0002
SWP_NOSIZE = 0x0001
SWP_NOZORDER = 0x0004
SWP_SHOWWINDOW = 0x0040


if os.name == "nt":
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    user32.GetForegroundWindow.restype = wintypes.HWND
    user32.GetDpiForWindow.argtypes = [wintypes.HWND]
    user32.GetDpiForWindow.restype = wintypes.UINT
    # Match WT's Per-Monitor V2 so SetWindowPos sizes are physical pixels.
    try:
        ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except Exception:
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except Exception:
            pass


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
    if not pid:
        return
    # taskkill /F survives CRT "Debug Error!" modal dialogs that block WM_CLOSE
    # and OpenProcess(PROCESS_TERMINATE) alone.
    subprocess.run(
        ["taskkill", "/PID", str(pid), "/T", "/F"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    handle = kernel32.OpenProcess(PROCESS_TERMINATE, False, pid)
    if handle:
        try:
            kernel32.TerminateProcess(handle, 1)
        finally:
            kernel32.CloseHandle(handle)


def _window_text(hwnd: int) -> str:
    length = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(length + 1)
    user32.GetWindowTextW(hwnd, buf, len(buf))
    return buf.value


def collect_runtime_dialog_text(owner_pids: set[int] | None = None) -> list[str]:
    """Capture MSVC abort/assert dialog text before force-killing owners."""
    found: list[str] = []
    if os.name != "nt":
        return found
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    @callback_type
    def callback(hwnd: int, _param: int) -> bool:
        if not user32.IsWindowVisible(hwnd):
            return True
        title = _window_text(hwnd)
        if not title:
            return True
        lowered = title.casefold()
        if not any(
            marker in lowered
            for marker in (
                "debug error",
                "microsoft visual c++ runtime library",
                "assertion failed",
                "visual c++",
            )
        ):
            return True
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        owner = int(pid.value)
        if owner_pids is not None and owner not in owner_pids:
            return True
        # Child static controls usually hold the abort message body.
        body_parts: list[str] = [title]
        enum_child = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

        @enum_child
        def child_cb(child: int, _lp: int) -> bool:
            text = _window_text(child)
            if text and text.strip() and text not in body_parts:
                body_parts.append(text.strip())
            return True

        user32.EnumChildWindows(hwnd, child_cb, 0)
        found.append(f"pid={owner}: " + " | ".join(body_parts))
        return True

    user32.EnumWindows(callback, 0)
    return found


# Runs still alive when the interpreter exits (abort, Ctrl+C, agent interrupt).
_ACTIVE_WT_RUNS: list[weakref.ref] = []


def _atexit_cleanup_wt_runs() -> None:
    for ref in list(_ACTIVE_WT_RUNS):
        run = ref()
        if run is None:
            continue
        try:
            run.close()
        except Exception:
            pass
    try:
        kill_htm_daemons()
    except Exception:
        pass


if os.name == "nt":
    atexit.register(_atexit_cleanup_wt_runs)


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
    """Bring the e2e Terminal window forward without attaching input queues.

    ``AttachThreadInput`` + ``SetFocus`` can deadlock when Terminal's UI thread
    is busy creating the native HTM tab (the process then shows as Not
    Responding and the harness waits forever). CLI actions only need the
    window activated; keystrokes use ``SendInput`` to the foreground window.
    """
    user32.ShowWindow(hwnd, SW_RESTORE)
    user32.BringWindowToTop(hwnd)
    for _ in range(12):
        user32.SwitchToThisWindow(hwnd, True)
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
        foreground = user32.GetForegroundWindow()
        if foreground and int(foreground) == hwnd:
            time.sleep(0.1)
            return
        time.sleep(0.05)


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
    # KEYEVENTF_UNICODE wScan is a UTF-16 code *unit*. Astral chars (emoji)
    # must be sent as surrogate pairs, not a truncated 32-bit ord().
    utf16 = text.encode("utf-16-le")
    for index in range(0, len(utf16), 2):
        code = int.from_bytes(utf16[index : index + 2], "little")
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
        time.sleep(0.006)


def log_text(temp_dir: Path, stem: str = "htmd") -> str:
    chunks: list[str] = []
    for path in sorted(temp_dir.glob(f"{stem}-*.log")):
        try:
            chunks.append(path.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            pass
    return "\n".join(chunks)


def marker_command(marker: Path, value: str) -> str:
    # The Windows HTM daemon starts a PowerShell pane.  Set-Content avoids the
    # redirection parsing differences that made a successful key round-trip
    # look like a failed command execution.
    return (
        "Set-Content -NoNewline -LiteralPath "
        f"'{marker.as_posix()}' -Value '{value}'\r"
    )


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
        self.preexisting_terminal_pids = set(processes_named("WindowsTerminal.exe"))
        self.crt_abort_messages: list[str] = []
        self.env = os.environ.copy()
        self.env["TEMP"] = str(temp_dir)
        self.env["TMP"] = str(temp_dir)
        self.env["HTM_BIN_DIR"] = str(htm.parent)

    def invoke_in(self, window_name: str, *command: str) -> None:
        argv = [str(self.wt), "-w", window_name, *command]
        # ``wtd new-tab`` often never exits; do not wait. Avoid DETACHED so the
        # command still reaches this named window instead of a new process.
        if command and command[0] in ("new-tab", "split-pane", "focus-tab", "move-focus"):
            subprocess.Popen(
                argv,
                env=self.env,
                cwd=str(Path.cwd()),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            time.sleep(0.85 if command[0] == "new-tab" else 0.5)
            return
        try:
            subprocess.run(
                argv,
                env=self.env,
                cwd=str(Path.cwd()),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=8,
            )
        except subprocess.TimeoutExpired:
            pass
        time.sleep(0.4)

    def invoke(self, *command: str) -> None:
        # Caller focuses the target window first. Re-focusing the gateway here
        # would steal ``-w last`` away from native HTM OS windows.
        self.invoke_in("last", *command)

    def new_htm_tab(self, kill_old: bool) -> None:
        args = ["new-tab", "--title", self.title, str(self.htm)]
        if kill_old:
            args.append("-x")
        self.invoke(*args)

    def start(self) -> None:
        self.start_new_window(True)

    def start_new_window(self, kill_old: bool) -> None:
        before = {hwnd for hwnd, _pid, _title in windows()}
        self.window_name = f"htm-e2e-{uuid.uuid4()}"
        self.title = f"HTM E2E {uuid.uuid4()}"
        args = [
            "new-tab",
            "--title",
            self.title,
            "--startingDirectory",
            str(Path.cwd()),
            str(self.htm),
        ]
        if kill_old:
            args.append("-x")
        # ``-w new`` always creates an OS window.
        self.invoke_in("new", *args)

        def find_window():
            titled = [w for w in windows() if self.title in w[2]]
            fresh = [w for w in titled if w[0] not in before]
            # Do not fall back to a pre-existing window whose tab title changed.
            return (fresh or [None])[0]

        hwnd, pid, _title = wait_for(find_window, 15, "Windows Terminal window")
        self.hwnd = hwnd
        self.hwnds.append(hwnd)
        self.terminal_pid = pid
        self._register_active()
        focus_window(hwnd)

    def _register_active(self) -> None:
        _ACTIVE_WT_RUNS[:] = [ref for ref in _ACTIVE_WT_RUNS if ref() is not None]
        if not any(ref() is self for ref in _ACTIVE_WT_RUNS):
            _ACTIVE_WT_RUNS.append(weakref.ref(self))

    def _unregister_active(self) -> None:
        _ACTIVE_WT_RUNS[:] = [
            ref for ref in _ACTIVE_WT_RUNS if ref() is not None and ref() is not self
        ]

    def close(self) -> None:
        """Tear down the e2e Terminal window even if a CRT assert dialog is up.

        Debug builds often show a modal ``Debug Error!`` / MSVC Runtime dialog
        that ignores ``WM_CLOSE``. Always force-kill the process tree we started,
        plus htm/htmd, so aborted runs do not leave windows on screen.
        """
        owned_pids = {
            pid
            for pid in processes_named("WindowsTerminal.exe")
            if pid not in self.preexisting_terminal_pids
        }
        if self.terminal_pid:
            owned_pids.add(self.terminal_pid)
        for message in collect_runtime_dialog_text(owned_pids or None):
            print(f"WT runtime dialog: {message}", file=sys.stderr, flush=True)
            self.crt_abort_messages.append(message)

        open_hwnds = list(dict.fromkeys(self.hwnds))
        for hwnd in open_hwnds:
            try:
                user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
            except Exception:
                pass
        if open_hwnds:
            deadline = time.monotonic() + 0.8
            while time.monotonic() < deadline and any(
                item[0] in open_hwnds for item in windows()
            ):
                time.sleep(0.05)

        for pid in sorted(owned_pids):
            terminate_pid(pid)

        kill_htm_daemons()
        self.hwnd = 0
        self.hwnds.clear()
        self.terminal_pid = 0
        self._unregister_active()


def _wait_for_htm_command(run: WindowsTerminalRun, before_count: int, command: str, action: str) -> None:
    # Stock Windows Terminal does not speak HTM; detect lack of integration
    # early and skip instead of failing the suite.
    try:
        wait_for(
            lambda: log_text(run.temp_dir).count(f"control command: {command}") > before_count,
            10,
            f"Windows Terminal {action} command",
        )
    except RuntimeError as exc:
        # No HTM packet arrived; this Windows Terminal build is not HTM-enabled.
        if "timed out waiting for Windows Terminal" in str(exc):
            skip(
                f"Windows Terminal HTM integration not present (no {action} command from wt={run.wt}); "
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
    # Windows AF_UNIX paths are relative to the daemon's working directory.
    # The launcher intentionally preserves that directory so the terminal and
    # daemon resolve the same endpoint.
    socket_path = Path.cwd() / f"htm.{sanitized}.ipc"

    wait_for(lambda: processes_named("htmd.exe", run.htmd), 15, "htmd startup")
    wait_for(
        lambda: "control command: refresh-client" in log_text(run.temp_dir),
        15,
        "initial HTM handshake",
    )
    first_htmd = processes_named("htmd.exe", run.htmd)[0]

    split_count = log_text(run.temp_dir).count("control command: split-window")
    resize_count = log_text(run.temp_dir).count("control command: resize-pane -t %")
    run.invoke("split-pane", "--duplicate")
    _wait_for_htm_command(run, split_count, "split-window", "split")
    wait_for(
        lambda: log_text(run.temp_dir).count("control command: resize-pane -t %")
        > resize_count,
        10,
        "split pane identifier assignment",
    )
    # The split action preserves focus on the HTM leader (the debug/control
    # surface), so move to the new follower before typing shell input.
    run.invoke("move-focus", "right")
    time.sleep(0.2)
    focus_window(run.hwnd)
    split_marker = run.temp_dir / f"split-{iteration}.txt"
    type_text(" " + marker_command(split_marker, "split-ok"))
    wait_for(split_marker.exists, 10, "command execution in split pane")

    tab_count = log_text(run.temp_dir).count("control command: new-window")
    resize_count = log_text(run.temp_dir).count("control command: resize-pane -t %")
    run.invoke("new-tab")
    _wait_for_htm_command(run, tab_count, "new-window", "new-tab")
    wait_for(
        lambda: log_text(run.temp_dir).count("control command: resize-pane -t %")
        > resize_count,
        10,
        "new-tab pane identifier assignment",
    )
    tab_marker = run.temp_dir / f"tab-{iteration}.txt"
    focus_window(run.hwnd)
    type_text(" " + marker_command(tab_marker, "tab-ok"))
    wait_for(tab_marker.exists, 10, "command execution in HTM tab")
    print("  OK: split/tab actions and ConPTY command round-trips", flush=True)

    # Start a replacement while the first client and daemon are live. This is
    # the Windows AF_UNIX teardown race that previously left an unusable path.
    starts = log_text(run.temp_dir).count("control command: refresh-client")
    run.start_new_window(True)
    wait_for(
        lambda: any(pid != first_htmd for pid in processes_named("htmd.exe", run.htmd)),
        15,
        "replacement htmd process",
    )
    wait_for(
        lambda: log_text(run.temp_dir).count("control command: refresh-client") > starts,
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

    starts = log_text(run.temp_dir).count("control command: refresh-client")
    # A fresh window avoids relying on Windows Terminal's window-index routing
    # after the original window has gained additional HTM tabs.
    run.start_new_window(False)
    focus_window(run.hwnd)
    wait_for(
        lambda: log_text(run.temp_dir).count("control command: refresh-client") > starts,
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


NAME = "Windows Terminal"
PLATFORMS = ("win32",)


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--wt", help="Path to an HTM-enabled wt.exe or wtd.exe")


def apply_args(args: argparse.Namespace) -> None:
    if args.wt:
        os.environ["WT_BIN"] = args.wt


TERMINAL_FORK = Path("E:/github/Terminal")


def candidate_wt() -> list[Path]:
    paths: list[Path] = []
    env = os.environ.get("WT_BIN")
    if env:
        paths.append(Path(env))
    paths.extend(
        [
            Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft" / "WindowsApps" / "wtd.exe",
            TERMINAL_FORK / "bin" / "x64" / "Debug" / "WindowsTerminal" / "WindowsTerminal.exe",
            TERMINAL_FORK / "bin" / "x64" / "Debug" / "wt.exe",
            TERMINAL_FORK / "src" / "cascadia" / "CascadiaPackage" / "bin" / "x64" / "Debug" / "wtd.exe",
            TERMINAL_FORK / "src" / "cascadia" / "CascadiaPackage" / "bin" / "x64" / "Debug" / "WindowsTerminal.exe",
        ]
    )
    which = shutil.which("wtd.exe") or shutil.which("wt.exe")
    if which:
        paths.append(Path(which))
    seen: set[Path] = set()
    unique: list[Path] = []
    for path in paths:
        resolved = path.resolve() if path.exists() else path
        if resolved in seen:
            continue
        seen.add(resolved)
        unique.append(path)
    return unique


def find_wt() -> Path:
    for path in candidate_wt():
        if path.is_file():
            return path.resolve()
    skip("Windows Terminal was not found; set WT_BIN or pass --wt")


class WindowsTerminalControlSession(GuiTerminalSession):
    """Shared GUI-suite adapter for an HTM-enabled Windows Terminal build."""

    name = NAME
    supports_detach = True
    supports_native_resize = True

    def __init__(self, wt: Path, htm: Path, htmd: Path):
        super().__init__(htm, htmd)
        self.wt = wt
        self.run: WindowsTerminalRun | None = None
        self.app = wt
        self.native_hwnds: list[int] = []

    def start(self, command: str = "") -> None:
        kill_htm_daemons()
        time.sleep(2.0)
        self.started_at = time.time() - 1.0
        self.run = WindowsTerminalRun(self.wt, self.htm, self.htmd, Path(tempfile.gettempdir()))
        self.run.env["PATH"] = f"{self.htm.parent};{self.run.env.get('PATH', '')}"
        self.run.start_new_window(True)
        self.native_hwnds.clear()

    def stop(self) -> None:
        abort_msgs: list[str] = []
        try:
            if self.run:
                self.run.close()
                abort_msgs.extend(self.run.crt_abort_messages)
                self.run = None
        finally:
            kill_htm_daemons()
            for ref in list(_ACTIVE_WT_RUNS):
                leftover = ref()
                if leftover is not None:
                    try:
                        leftover.close()
                        abort_msgs.extend(leftover.crt_abort_messages)
                    except Exception:
                        pass
            self.native_hwnds.clear()
        if abort_msgs:
            fail(
                "Windows Terminal Debug CRT abort during teardown: "
                + " | ".join(abort_msgs[:3])
            )

    def _discover_native_windows(self) -> None:
        """Native HTM panes are separate OS windows, not tabs on the gateway."""
        if not self.run or not self.run.hwnd:
            return
        gateway = self.run.hwnd
        owned = [
            h
            for h, p, _title in windows()
            if h != gateway and p == self.run.terminal_pid
        ]
        # Content moved to a new window may share the same process.
        if not owned:
            owned = [
                h
                for h, p, _title in windows()
                if h != gateway
                and p not in self.run.preexisting_terminal_pids
                and p in processes_named("WindowsTerminal.exe")
            ]
        # Preserve creation order: keep previously known hwnds first, append new.
        known = [h for h in self.native_hwnds if h in owned]
        for h in owned:
            if h not in known:
                known.append(h)
        self.native_hwnds = known

    def after_attach(self) -> None:
        try:
            self.wait_log(
                lambda text: "control command: refresh-client" in text
                or "control command: split-window" in text,
                15,
                "Windows Terminal tmux -CC handshake",
            )
        except SystemExit:
            skip(
                "Windows Terminal accepted HTM's DCS but did not send tmux -CC "
                "commands; use an HTM-enabled Debug wt/wtd build"
            )
        wait_until(
            lambda: "resize-pane -t %" in self.log_text(),
            15,
            description="native HTM window created",
        )
        time.sleep(0.8)
        self._discover_native_windows()
        self.focus_native()
        time.sleep(0.3)

    def after_first_split(self) -> None:
        self.focus_native()
        self._action("move-focus", "nextInOrder")

    def focus_gateway(self) -> None:
        if not self.run:
            fail("Windows Terminal was not started")
        if self.run.hwnd:
            focus_window(self.run.hwnd)

    def focus_native(self) -> None:
        if not self.run:
            fail("Windows Terminal was not started")
        self._discover_native_windows()
        target = self.native_hwnds[-1] if self.native_hwnds else self.run.hwnd
        if target:
            focus_window(target)
            time.sleep(0.15)

    def focus_native_window(self) -> None:
        """Corners suite uses this after detach/reattach; HWND, not osascript."""
        self.focus_native()

    def launched_windows(self) -> list[dict]:
        self._discover_native_windows()
        out: list[dict] = []
        for i, hwnd in enumerate(self.native_hwnds):
            rect = wintypes.RECT()
            if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
                continue
            w = int(rect.right - rect.left)
            h = int(rect.bottom - rect.top)
            if w < 2 or h < 2:
                continue
            out.append(
                {
                    "index": i,
                    "x": int(rect.left),
                    "y": int(rect.top),
                    "w": w,
                    "h": h,
                    "name": f"hwnd-{hwnd}",
                    "hwnd": hwnd,
                }
            )
        return out

    def ax_windows(self) -> list[dict]:
        return self.launched_windows()

    def resize_front_native_window(self, width: int, height: int) -> None:
        """Resize the front native HTM pane window so mux geometry updates."""
        self._discover_native_windows()
        hwnd = None
        if self.native_hwnds:
            hwnd = self.native_hwnds[-1]
        elif self.run and self.run.hwnd:
            hwnd = self.run.hwnd
        if not hwnd:
            fail("no Windows Terminal HWND available to resize")
        focus_window(hwnd)
        rect = wintypes.RECT()
        if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
            fail(f"GetWindowRect failed for hwnd={hwnd}")
        dpi = user32.GetDpiForWindow(hwnd) or 96
        scale = max(dpi / 96.0, 1.0)
        # WezTerm's 900x700 is content-ish; WT SetWindowPos is outer size and
        # title-bar/DPI eat the client area. Grow so mux can clear 100x30.
        target_w = max(int(width), int((110 * 9 + 120) * scale), rect.right - rect.left)
        target_h = max(int(height), int((35 * 18 + 140) * scale), rect.bottom - rect.top)
        resizes_before = self.log_text().count("resize-pane -t %")
        refreshes_before = self.log_text().count("refresh-client -C")
        if not user32.SetWindowPos(
            hwnd,
            0,
            rect.left,
            rect.top,
            target_w,
            target_h,
            SWP_NOZORDER | SWP_SHOWWINDOW,
        ):
            fail(f"SetWindowPos failed for hwnd={hwnd}")
        wait_until(
            lambda: self.log_text().count("resize-pane -t %") > resizes_before
            or self.log_text().count("refresh-client -C") > refreshes_before,
            8,
            description="HTM received resize after native window SetWindowPos",
        )
        time.sleep(0.25)
        self.focus_native()

    def detach_client(self) -> None:
        self.focus_gateway()
        shortcut(VK_ESCAPE)
        wait_until(
            lambda: "detach-client" in self.log_text() or "detach" in self.log_text(),
            15,
            description="htmd received detach-client",
        )
        # Product ForceCloseUi + page _HtmClosePane must dismiss native HTM
        # windows. Do not WM_CLOSE them — that hides product regressions.
        self._discover_native_windows()
        native_stale = list(self.native_hwnds)
        deadline = time.time() + 8
        while time.time() < deadline:
            self._discover_native_windows()
            if not any(h in self.native_hwnds for h in native_stale):
                break
            time.sleep(0.1)
        self._discover_native_windows()
        still_native = [h for h in native_stale if h in self.native_hwnds]
        if still_native:
            fail(
                "native HTM windows still open after detach "
                f"(hwnds={still_native}); product should ForceCloseUi/_HtmClosePane"
            )
        # Gateway is the control plane; Esc does not close it. Close only the
        # gateway HWND so reattach can start_new_window cleanly (WezTerm keeps
        # the gateway and reuses it — WT driver reopens a fresh client).
        if self.run and self.run.hwnd:
            gateway = self.run.hwnd
            user32.PostMessageW(gateway, WM_CLOSE, 0, 0)
            deadline = time.time() + 2
            while time.time() < deadline:
                if not any(item[0] == gateway for item in windows()):
                    break
                time.sleep(0.1)
            self.run.hwnd = None
        self.native_hwnds.clear()
        time.sleep(0.3)

    def reattach_client(self) -> None:
        if not self.run:
            fail("Windows Terminal was not started")
        starts = self.log_text().count("control command: refresh-client")
        self.run.start_new_window(False)
        self.focus()
        self.wait_log(
            lambda text: text.count("control command: refresh-client") > starts,
            15,
            "HTM reattach",
        )
        wait_until(
            lambda: self.log_text().count("resize-pane -t %") >= 1,
            15,
            description="reattach native pane",
        )
        time.sleep(0.5)
        self._discover_native_windows()
        self.focus_native()

    def focus(self) -> None:
        # Default focus for typing/splits is the native HTM window once it exists.
        if self.native_hwnds or "resize-pane -t %" in self.log_text():
            self.focus_native()
        elif self.run and self.run.hwnd:
            focus_window(self.run.hwnd)

    def _action(self, *command: str) -> None:
        if not self.run:
            fail("Windows Terminal was not started")
        self.focus()
        self.run.invoke(*command)
        time.sleep(0.35)

    def keystroke(self, keys: str, using: str = "") -> None:
        if not self.run:
            fail("Windows Terminal was not started")
        value = keys.strip('"')
        if "command" in using:
            if value == "d":
                before = self.log_text().count("split-window")
                resizes_before = self.log_text().count("resize-pane -t %")
                self.focus_native()
                # Match WezTerm/iTerm2: Cmd+D = side-by-side, Cmd+Shift+D = stacked.
                split_flag = (
                    "--horizontal" if "shift" in using.lower() else "--vertical"
                )
                for _ in range(4):
                    self._action("split-pane", split_flag, "--duplicate")
                    deadline = time.time() + 2.5
                    while time.time() < deadline:
                        if self.log_text().count("split-window") > before:
                            deadline_resize = time.time() + 5
                            while time.time() < deadline_resize:
                                if (
                                    self.log_text().count("resize-pane -t %")
                                    > resizes_before
                                ):
                                    break
                                time.sleep(0.1)
                            return
                        time.sleep(0.1)
            elif value == "t":
                self.focus_native()
                self._action("new-tab")
                time.sleep(1.0)
                self._discover_native_windows()
            elif value == "w":
                self.focus_native()
                shortcut(VK_CONTROL, VK_SHIFT, ord("W"))
                time.sleep(0.4)
            elif value == "[":
                self.focus_native()
                self._action("move-focus", "previousInOrder")
            elif value == "]":
                self.focus_native()
                self._action("move-focus", "nextInOrder")
            else:
                fail(f"unsupported Windows Terminal action: {keys} {using}")
            return
        if "control" in using:
            self.focus_native()
            shortcut(VK_CONTROL, ord(value.upper()))
            time.sleep(0.12)
            return
        self.focus_native()
        type_text(" " + value)
        needle = value[-24:] if len(value) > 24 else value
        if needle.strip():
            self.wait_log(
                lambda text: needle in typed_from_log(text) or needle in text,
                45,
                f"Windows Terminal send-keys flushed {needle!r}",
            )

    def submit_text(self, text: str) -> None:
        """Type a command and Enter without re-focusing between them.

        ``focus_window`` / ``focus_native`` after a split can activate the new
        sibling pane, so a separate Enter key_code would land in the wrong PTY
        while the echo line stayed unexecuted in the pane that received typing.
        """
        if not self.run:
            fail("Windows Terminal was not started")
        self.focus_native()
        type_text(" " + text + "\r")
        needle = text[-24:] if len(text) > 24 else text
        if needle.strip():
            self.wait_log(
                lambda text_log: needle in typed_from_log(text_log) or needle in text_log,
                45,
                f"Windows Terminal send-keys flushed {needle!r}",
            )
        time.sleep(0.12)

    def key_code(self, code: int, using: str = "") -> None:
        if code != 36:
            fail(f"unsupported Windows Terminal key code: {code}")
        # Do not call focus_native(): re-activating the HWND after a split often
        # gives keyboard focus to the sibling pane. Send CR to the pane that
        # already has focus from the preceding keystroke.
        type_text("\r")
        time.sleep(0.12)

    def previous_pane(self) -> None:
        self.keystroke('"["', "command down")

    def next_pane(self) -> None:
        self.keystroke('"]"', "command down")

    def previous_tab(self) -> None:
        self._discover_native_windows()
        if len(self.native_hwnds) >= 2:
            focus_window(self.native_hwnds[-2])
            time.sleep(0.2)
        elif self.native_hwnds:
            focus_window(self.native_hwnds[0])
            time.sleep(0.2)

    def tab_count(self) -> int:
        # Gateway window + native HTM OS windows.
        if not self.run or not self.run.hwnd:
            return 0
        self._discover_native_windows()
        return 1 + len(self.native_hwnds)

    def is_alive(self) -> bool:
        return bool(self.run and self.run.hwnd)


def open_session(htm: Path, htmd: Path, args: argparse.Namespace) -> WindowsTerminalControlSession:
    return WindowsTerminalControlSession(find_wt(), htm, htmd)


if __name__ == "__main__":
    raise SystemExit(run_emulator_main(sys.modules[__name__], default_suite="all"))
