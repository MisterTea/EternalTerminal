#!/usr/bin/env python3
"""End-to-end tests for htm/htmd native WezTerm integration.

Launches WezTerm with an isolated ``--config-file`` so the user's
``~/.wezterm.lua`` is left alone. Prefers a locally built binary
(``~/github/wezterm/target/release/wezterm``) over ``/Applications/WezTerm.app``.
Protocol correctness is checked against htmd logs; native splits/tabs are
driven with the same Cmd+D / Cmd+T / Cmd+Shift+D keys as the iTerm2 suite.

Skip (exit 77) when WezTerm, htm/htmd, or Accessibility is unavailable.
Not registered with default CTest; run this file directly (see AGENTS.md).
Expects WezTerm's tmux -CC integration (DCS 1000p from ``htm``).

Environment:
  WEZTERM_BIN  Path to a ``wezterm`` / ``wezterm-gui`` binary
  WEZTERM_APP  Path to WezTerm.app (default: /Applications/WezTerm.app)
  HTM_BIN      Path to the ``htm`` binary (overridden by --htm)
  HTMD_BIN     Path to the ``htmd`` binary (overridden by --htmd)
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

from htm_gui_e2e import (  # noqa: E402
    GuiTerminalSession,
    ScreenRecorder,
    _window_key,
    command_count,
    control_commands,
    fail,
    ipc_path,
    kill_htm_daemons,
    log_has_typed,
    pids_named,
    run_emulator_main,
    run_osascript,
    skip,
    typed_from_log,
    wait_until,
)

HERE = Path(__file__).resolve().parent
CONFIG_LUA = HERE / "wezterm_htm_e2e.lua"
STDERR_LOG = Path(tempfile.gettempdir() if os.name == "nt" else "/tmp") / "wezterm-htm-e2e.stderr"
WEZTERM_FORK = Path("e:/github/wezterm")
PLATFORMS = ("darwin", "win32")
NAME = "WezTerm"


def wezterm_stderr_excerpt() -> str:
    try:
        text = STDERR_LOG.read_text(errors="replace")
    except OSError:
        return ""
    for needle in (
        "local task polled by a thread that didn't spawn it",
        "tmux -CC mode requested",
        "Unrecognized tmux cc line",
    ):
        idx = text.find(needle)
        if idx >= 0:
            start = max(0, idx - 120)
            return text[start : idx + 280]
    return text[-1200:]


def fail_wezterm_crash(when: str) -> None:
    fail(
        f"WezTerm exited {when}. tmux -CC attach may have panicked "
        "(https://github.com/wezterm/wezterm/issues/336). "
        f"stderr:\n{wezterm_stderr_excerpt()}"
    )


def is_wezterm_app(app: Path) -> bool:
    macos = app / "Contents" / "MacOS"
    return (macos / "wezterm-gui").is_file() or (macos / "wezterm").is_file()


def is_wezterm_binary(path: Path) -> bool:
    return (
        path.is_file()
        and os.access(path, os.X_OK)
        and "wezterm" in path.name
    )


def candidate_wezterm() -> list[Path]:
    paths: list[Path] = []
    for env in ("WEZTERM_BIN", "WEZTERM_APP"):
        val = os.environ.get(env)
        if val:
            paths.append(Path(val))
    home = Path.home()
    exe = ".exe" if os.name == "nt" else ""
    paths.extend(
        [
            WEZTERM_FORK / "target" / "release" / f"wezterm-gui{exe}",
            WEZTERM_FORK / "target" / "release" / f"wezterm{exe}",
            home / "github" / "wezterm" / "target" / "release" / f"wezterm-gui{exe}",
            home / "github" / "wezterm" / "target" / "release" / f"wezterm{exe}",
            home / "github" / "wezterm" / "target" / "debug" / f"wezterm{exe}",
            Path("/Applications/WezTerm.app"),
        ]
    )
    seen = set()
    unique: list[Path] = []
    for path in paths:
        resolved = path.resolve() if path.exists() else path
        if resolved in seen:
            continue
        seen.add(resolved)
        unique.append(path)
    return unique


def find_wezterm_app() -> Path:
    for path in candidate_wezterm():
        if path.is_dir() and is_wezterm_app(path):
            return path
        if is_wezterm_binary(path):
            return path
    skip(
        "no WezTerm found; build ~/github/wezterm or set WEZTERM_BIN / WEZTERM_APP"
    )


def pid_comm(pid: int) -> str:
    try:
        return subprocess.check_output(
            ["ps", "-p", str(pid), "-o", "comm="],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except subprocess.CalledProcessError:
        return ""


def child_pids(pid: int) -> list[int]:
    try:
        out = subprocess.check_output(
            ["pgrep", "-P", str(pid)],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except subprocess.CalledProcessError:
        return []
    return [int(line) for line in out.split() if line.isdigit()]


def descendant_pids(root: int) -> list[int]:
    found: list[int] = []
    stack = [root]
    seen = {root}
    while stack:
        pid = stack.pop()
        for child in child_pids(pid):
            if child in seen:
                continue
            seen.add(child)
            found.append(child)
            stack.append(child)
    return found


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--wezterm")
    parser.add_argument("--wezterm-app")


def apply_args(args: argparse.Namespace) -> None:
    if args.wezterm:
        os.environ["WEZTERM_BIN"] = args.wezterm
    if args.wezterm_app:
        os.environ["WEZTERM_APP"] = args.wezterm_app
    if not CONFIG_LUA.is_file():
        fail(f"missing {CONFIG_LUA}")


def open_session(htm: Path, htmd: Path, args: argparse.Namespace):
    app = find_wezterm_app()
    if os.name == "nt":
        return WindowsWezTermHtmSession(app, htm, htmd)
    return WezTermHtmSession(app, htm, htmd)


if os.name == "nt":
    import ctypes
    from ctypes import wintypes
    from windows_terminal_htm_e2e import (  # noqa: E402
        VK_CONTROL,
        VK_MENU,
        VK_RETURN,
        VK_SHIFT,
        KEYEVENTF_KEYUP,
        focus_window,
        processes_named,
        send_key,
        shortcut,
        terminate_pid,
        type_text,
        windows,
    )

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    VK_TAB = 0x09
    WEZTERM_CLASS = "org.wezfurlong.wezterm"
    MOUSEEVENTF_LEFTDOWN = 0x0002
    MOUSEEVENTF_LEFTUP = 0x0004

    def type_vk_text(text: str) -> None:
        for char in text:
            packed = int(user32.VkKeyScanW(ord(char)))
            if packed in (-1, 0xFFFF, 0xFFFFFFFF):
                type_text(char)
                time.sleep(0.012)
                continue
            vk = packed & 0xFF
            mods: list[int] = []
            if packed & 0x200:
                mods.append(VK_CONTROL)
            if packed & 0x100:
                mods.append(VK_SHIFT)
            shortcut(*mods, vk)
            time.sleep(0.012)

    class WindowsWezTermHtmSession(GuiTerminalSession):
        name = NAME
        supports_detach = False
        supports_native_resize = False

        def __init__(self, app: Path, htm: Path, htmd: Path):
            super().__init__(htm, htmd)
            self.app = app
            self.proc: Optional[subprocess.Popen] = None
            self.hwnd = 0
            self.gateway_hwnd = 0
            self.stderr_file = None
            self.preexisting = set(processes_named("wezterm-gui.exe")) | set(
                processes_named("wezterm.exe")
            )

        def wezterm_bin(self) -> Path:
            gui = self.app.with_name("wezterm-gui.exe")
            if self.app.name.casefold() == "wezterm.exe" and gui.is_file():
                return gui
            return self.app

        def _owned_pids(self) -> set[int]:
            return (
                set(processes_named("wezterm-gui.exe"))
                | set(processes_named("wezterm.exe"))
            ) - self.preexisting

        def _window_class(self, hwnd: int) -> str:
            buf = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, buf, 256)
            return buf.value

        def _owned_windows(self) -> list[tuple[int, str]]:
            pids = self._owned_pids()
            found: list[tuple[int, str]] = []
            for hwnd, pid, title in windows():
                if pid not in pids:
                    continue
                if self._window_class(hwnd) != WEZTERM_CLASS:
                    continue
                found.append((hwnd, title))
            return found

        def _owned_hwnds(self) -> list[int]:
            return [hwnd for hwnd, _title in self._owned_windows()]

        def ax_windows(self) -> list[dict]:
            rect = wintypes.RECT()
            out: list[dict] = []
            for index, (hwnd, title) in enumerate(self._owned_windows(), start=1):
                if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
                    continue
                out.append(
                    {
                        "index": index,
                        "name": title,
                        "x": float(rect.left),
                        "y": float(rect.top),
                        "w": float(max(0, rect.right - rect.left)),
                        "h": float(max(0, rect.bottom - rect.top)),
                        "id": int(hwnd),
                    }
                )
            return out

        def launched_windows(self) -> list[dict]:
            return [win for win in self.ax_windows() if win["w"] >= 64 and win["h"] >= 64]

        def _remember_window(self) -> bool:
            hwnds = self._owned_hwnds()
            if not hwnds:
                return False
            titled = self._owned_windows()
            for hwnd, title in titled:
                if "htm" in title.lower():
                    self.gateway_hwnd = hwnd
                    break
            else:
                if not self.gateway_hwnd:
                    self.gateway_hwnd = hwnds[0]
            self.hwnd = self.gateway_hwnd or hwnds[0]
            return True

        def _native_hwnd(self) -> int:
            for hwnd, title in self._owned_windows():
                lower = title.lower()
                if "cmd.exe" in lower or "powershell" in lower or "pwsh" in lower:
                    return hwnd
            hwnds = self._owned_hwnds()
            natives = [hwnd for hwnd in hwnds if hwnd != self.gateway_hwnd]
            if natives:
                return natives[-1]
            return self.gateway_hwnd or self.hwnd

        def _release_modifiers(self) -> None:
            for vk in (VK_SHIFT, VK_CONTROL, VK_MENU):
                send_key(vk, KEYEVENTF_KEYUP)

        def _click_hwnd(self, hwnd: int) -> None:
            rect = wintypes.RECT()
            if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
                return
            x = (rect.left + rect.right) // 2
            y = (rect.top + rect.bottom) * 45 // 100
            user32.SetCursorPos(x, y)
            user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
            user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
            time.sleep(0.12)

        def focus_gateway(self) -> None:
            self._remember_window()
            if self.gateway_hwnd:
                focus_window(self.gateway_hwnd)

        def focus_native_window(self) -> None:
            native = self._native_hwnd()
            if not native:
                fail("could not identify the native WezTerm HTM window")
            self.hwnd = native
            focus_window(native)

        def focus(self) -> None:
            if self._native_hwnd() and self._native_hwnd() != self.gateway_hwnd:
                self.focus_native_window()
            else:
                self.focus_gateway()

        def _flush_needle(self, value: str) -> str:
            found: list[str] = []
            for token in value.replace("&", " ").split():
                if any(
                    part in token
                    for part in ("PARITY", "STBULK", "STKEY", "HTM_E2E")
                ):
                    found.append(token.split("%")[0].rstrip("_") or token)
            if found:
                return found[-1]
            return value[-16:] if len(value) > 16 else value

        def keystroke(self, keys: str, using: str = "") -> None:
            value = keys.strip('"')
            if "command" in using:
                self.focus_native_window()
                action = (value, "shift" in using)
                mapping = {
                    ("d", False): (VK_CONTROL, VK_SHIFT, ord("D")),
                    ("d", True): (VK_CONTROL, VK_MENU, ord("D")),
                    ("t", False): (VK_CONTROL, VK_SHIFT, ord("T")),
                    ("w", False): (VK_CONTROL, VK_SHIFT, ord("W")),
                    ("[", False): (VK_CONTROL, VK_SHIFT, ord("H")),
                    ("]", False): (VK_CONTROL, VK_SHIFT, ord("L")),
                    ("[", True): (VK_CONTROL, VK_SHIFT, VK_TAB),
                }
                chord = mapping.get(action)
                if chord is None:
                    fail(f"unsupported WezTerm Windows action: {keys} {using}")
                shortcut(*chord)
                time.sleep(0.2)
                self._release_modifiers()
                time.sleep(0.45)
                self.focus_native_window()
                return
            self.focus_native_window()
            self._release_modifiers()
            type_text(" " + value)
            needle = self._flush_needle(value)
            if len(value) < 28:
                return
            self.wait_log(
                lambda text: needle in typed_from_log(text) or needle in text,
                90,
                f"WezTerm send-keys flushed {needle!r}",
            )

        def mux_snapshot(self, wait: float = 2.0) -> str:
            text = self.log_text()
            return text + "\n" + typed_from_log(text)

        def htm_pane_snapshot(self, wait: float = 2.0) -> str:
            return self.mux_snapshot(wait)

        def wait_typed(self, marker: str, timeout: float = 20.0) -> None:
            super().wait_typed(marker, timeout=max(timeout, 45.0))

        def wait_visible(self, marker: str, timeout: float = 20.0) -> None:
            # htmd pane dumps use SIGUSR1, which Windows does not provide.
            self.wait_log(
                lambda text: log_has_typed(text, marker),
                max(timeout, 60.0),
                f"typed command producing {marker}",
            )
            time.sleep(0.5)

        def tab_count(self) -> int:
            native = self._native_hwnd()
            if native and native != self.gateway_hwnd:
                return 2
            return 1 if self.hwnd else 0

        def key_code(self, code: int, using: str = "") -> None:
            if code != 36:
                fail(f"unsupported WezTerm key code: {code}")
            self.focus_native_window()
            shortcut(VK_RETURN)
            time.sleep(0.12)

        def start(self, command: str = "") -> None:
            if not CONFIG_LUA.is_file():
                fail(f"missing WezTerm e2e config {CONFIG_LUA}")
            kill_htm_daemons()
            self.started_at = time.time() - 1.0
            env = os.environ.copy()
            env["PATH"] = f"{self.htm.parent};{self.wezterm_bin().parent};{env.get('PATH', '')}"
            env["WEZTERM_LOG"] = "info"
            STDERR_LOG.parent.mkdir(parents=True, exist_ok=True)
            self.stderr_file = open(STDERR_LOG, "w")
            self._release_modifiers()
            argv = [
                str(self.wezterm_bin()),
                "--config-file",
                str(CONFIG_LUA),
                "start",
                "--always-new-process",
                "--",
                str(self.htm),
                "-x",
            ]
            self.proc = subprocess.Popen(
                argv,
                env=env,
                cwd=str(self.htm.parent),
                stdout=subprocess.DEVNULL,
                stderr=self.stderr_file,
                creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
                | getattr(subprocess, "DETACHED_PROCESS", 0),
            )
            wait_until(
                lambda: self.proc is not None and self.proc.poll() is None,
                5,
                description="WezTerm process start",
            )
            wait_until(self._remember_window, 25, description="WezTerm window")
            print(
                f"gateway window hwnd={self.gateway_hwnd} "
                f"windows={self._owned_windows()}",
                flush=True,
            )
            self._release_modifiers()
            self.focus_gateway()
            self._release_modifiers()

        def after_attach(self) -> None:
            self.wait_log(
                lambda text: "list-panes" in text or "list-windows" in text,
                20,
                "WezTerm list-panes/list-windows",
            )
            def _native_ready() -> bool:
                self._release_modifiers()
                return len(self._owned_hwnds()) >= 2

            wait_until(
                _native_ready,
                20,
                description="HTM session in a separate WezTerm window",
            )
            details = self._owned_windows()
            self._remember_window()
            self._gateway_keys = {
                _window_key(win)
                for win in self.ax_windows()
                if int(win.get("id") or 0) == self.gateway_hwnd
            }
            print(f"WezTerm windows after attach: {details}", flush=True)
            time.sleep(1.0)
            if self.proc is not None and self.proc.poll() is not None:
                fail_wezterm_crash("after tmux -CC attach")
            self.focus_native_window()
            self._click_hwnd(self.hwnd)
            time.sleep(0.3)
            self._release_modifiers()

        def stop(self) -> None:
            self._release_modifiers()
            if self.proc and self.proc.poll() is None:
                try:
                    self.proc.terminate()
                except OSError:
                    pass
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    try:
                        self.proc.kill()
                    except OSError:
                        pass
            for pid in self._owned_pids():
                terminate_pid(pid)
            if self.stderr_file:
                try:
                    self.stderr_file.close()
                except OSError:
                    pass
                self.stderr_file = None
            kill_htm_daemons()

        def after_layout_suite(self) -> None:
            text = self.log_text()
            if command_count(text, "list-windows") < 1 and "list-windows" not in text:
                fail("WezTerm did not send list-windows after control-mode attach")
            print("OK: WezTerm enumerated tmux windows", flush=True)


class WezTermHtmSession(GuiTerminalSession):
    name = "WezTerm"
    supports_detach = True
    supports_native_resize = True

    def __init__(self, app: Path, htm: Path, htmd: Path):
        super().__init__(htm, htmd)
        self.app = app
        self.proc: Optional[subprocess.Popen] = None
        self.gui_pid: Optional[int] = None
        self.preexisting_gui = set(self._wezterm_gui_pids())
        self.stderr_file = None

    def _wezterm_gui_pids(self) -> list[int]:
        pids = []
        try:
            out = subprocess.check_output(["pgrep", "-x", "wezterm-gui"], text=True)
        except subprocess.CalledProcessError:
            return []
        for line in out.split():
            try:
                pids.append(int(line))
            except ValueError:
                continue
        return pids

    def _pid_command(self, pid: int) -> str:
        try:
            return subprocess.check_output(
                ["ps", "-p", str(pid), "-o", "command="],
                text=True,
                stderr=subprocess.DEVNULL,
            )
        except subprocess.CalledProcessError:
            return ""

    def wezterm_bin(self) -> Path:
        app = self.app
        if app.is_dir() and (app.suffix == ".app" or str(app).endswith(".app")):
            cli = app / "Contents" / "MacOS" / "wezterm"
            if cli.is_file():
                return cli
            return app / "Contents" / "MacOS" / "wezterm-gui"
        return app

    def resolve_gui_pid(self) -> int:
        if self.gui_pid and pid_comm(self.gui_pid):
            return self.gui_pid
        if self.proc and self.proc.poll() is None:
            comm = pid_comm(self.proc.pid)
            if "wezterm-gui" in comm or comm.endswith("wezterm-gui"):
                self.gui_pid = self.proc.pid
                return self.gui_pid
            for pid in descendant_pids(self.proc.pid):
                if "wezterm-gui" in pid_comm(pid):
                    self.gui_pid = pid
                    return pid
            self.gui_pid = self.proc.pid
            return self.gui_pid
        for pid in self._wezterm_gui_pids():
            if pid not in self.preexisting_gui:
                self.gui_pid = pid
                return pid
        fail("WezTerm GUI process is not running")

    @property
    def pid(self) -> int:
        return self.resolve_gui_pid()

    def osascript_pid(self, body: str) -> str:
        script = f'''
tell application "System Events"
  tell (first process whose unix id is {self.pid})
    {body}
  end tell
end tell
'''
        return run_osascript(script)

    def focus(self) -> None:
        try:
            self.osascript_pid("set frontmost to true")
        except subprocess.CalledProcessError:
            self.resolve_gui_pid()
            self.osascript_pid("set frontmost to true")
        time.sleep(0.15)

    def keystroke(self, keys: str, using: str = "") -> None:
        using_clause = f" using {using}" if using else ""
        self.osascript_pid(
            f"set frontmost to true\n    keystroke {keys}{using_clause}"
        )
        time.sleep(0.08)

    def key_code(self, code: int, using: str = "") -> None:
        using_clause = f" using {using}" if using else ""
        self.osascript_pid(
            f"set frontmost to true\n    key code {code}{using_clause}"
        )
        time.sleep(0.08)

    def click_screen(self, x: float, y: float) -> None:
        import ctypes
        import ctypes.util

        libname = ctypes.util.find_library("ApplicationServices")
        if not libname:
            fail("ApplicationServices is unavailable for WezTerm focus")
        cg = ctypes.cdll.LoadLibrary(libname)

        class CGPoint(ctypes.Structure):
            _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]

        cg.CGEventCreateMouseEvent.restype = ctypes.c_void_p
        cg.CGEventCreateMouseEvent.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            CGPoint,
            ctypes.c_uint32,
        ]
        cg.CGEventPost.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
        cg.CFRelease.argtypes = [ctypes.c_void_p]
        point = CGPoint(float(x), float(y))
        for event_type in (5, 1, 2):
            event = cg.CGEventCreateMouseEvent(None, event_type, point, 0)
            if not event:
                fail(f"CGEventCreateMouseEvent failed at {int(x)},{int(y)}")
            cg.CGEventPost(0, event)
            cg.CFRelease(event)
            time.sleep(0.05)

    def window_count(self) -> int:
        try:
            out = self.osascript_pid("get count of windows").strip()
            return int(out)
        except (ValueError, subprocess.CalledProcessError):
            return 0

    def wezterm_cli(self, *args: str) -> str:
        env = os.environ.copy()
        env["WEZTERM_UNIX_SOCKET"] = str(
            Path.home()
            / ".local"
            / "share"
            / "wezterm"
            / f"gui-sock-{self.resolve_gui_pid()}"
        )
        return subprocess.check_output(
            [str(self.wezterm_bin()), "cli", "--no-auto-start", *args],
            text=True,
            stderr=subprocess.STDOUT,
            env=env,
        )

    def gateway_pane(self) -> dict:
        panes = json.loads(self.wezterm_cli("list", "--format", "json"))
        candidates = [
            pane
            for pane in panes
            if pane.get("cursor_visibility") == "Hidden"
        ]
        if not candidates:
            fail(f"could not identify WezTerm {self.mux} gateway pane")
        return min(candidates, key=lambda pane: int(pane["pane_id"]))

    def gateway_pane_id(self) -> int:
        return int(self.gateway_pane()["pane_id"])

    def gateway_text(self) -> str:
        return self.wezterm_cli(
            "get-text",
            "--pane-id",
            str(self.gateway_pane_id()),
            "--start-line",
            "-10000",
        )

    def tab_count(self) -> int:
        scripts = [
            'count of (every radio button of tab group 1 of window 1)',
            'count of (every radio button of window 1)',
            'count of (every UI element of window 1 whose role is "AXRadioButton")',
        ]
        for body in scripts:
            try:
                out = self.osascript_pid(body).strip()
                count = int(out)
                if count > 0:
                    return count
            except (ValueError, subprocess.CalledProcessError):
                continue
        return 0

    def previous_pane(self) -> None:
        self.keystroke('"["', "command down")
        time.sleep(0.35)

    def next_pane(self) -> None:
        self.keystroke('"]"', "command down")
        time.sleep(0.35)

    def previous_tab(self) -> None:
        self.keystroke('"["', "{command down, shift down}")
        time.sleep(0.4)

    def multiplexer_command(self) -> str:
        command = super().multiplexer_command()
        shell = f'{command}; exec "${{SHELL:-/bin/zsh}}" -l'
        return f"/bin/sh -c {shlex.quote(shell)}"

    def remember_gateway_windows(self) -> None:
        windows = self.ax_windows()
        gateways = [
            window
            for window in windows
            if (window.get("name") or "").strip().lower() in {"~", "wezterm"}
        ]
        if not gateways:
            gateways = windows[:1]
        self._gateway_keys = {_window_key(window) for window in gateways}
        self._gateway_names = {
            (window.get("name") or "").strip() for window in gateways
        } - {""}
        self._gateway_clicks = [
            (
                float(window["x"]) + float(window["w"]) * 0.5,
                float(window["y"]) + float(window["h"]) * 0.45,
            )
            for window in gateways
        ]
        details = [
            f"{_window_key(window)}:{window.get('name') or '(unnamed)'}"
            for window in gateways
        ]
        print(f"gateway windows: {len(gateways)} {details}", flush=True)

    def remember_attached_gateway_window(self) -> None:
        windows = self.ax_windows()
        gateway_title = (self.gateway_pane().get("title") or "").strip()
        gateways = [
            window
            for window in windows
            if (window.get("name") or "").strip() == gateway_title
        ]
        if not gateways:
            fail(f"could not identify the attached WezTerm {self.mux} gateway")
        self._gateway_keys = {_window_key(window) for window in gateways}
        self._gateway_names = {
            (window.get("name") or "").strip() for window in gateways
        }
        self._gateway_clicks = [
            (
                float(window["x"]) + float(window["w"]) * 0.5,
                float(window["y"]) + float(window["h"]) * 0.45,
            )
            for window in gateways
        ]

    def launched_windows(self) -> list[dict]:
        return [
            window
            for window in self.ax_windows()
            if _window_key(window) not in self._gateway_keys
        ]

    def focus_native_window(self) -> None:
        launched = self.launched_windows()
        native = launched[0] if launched else None
        if native is None:
            fail(f"could not identify the native WezTerm {self.mux} window")
        self._front_native = native
        index = int(native["index"])
        self.osascript_pid(
            "set frontmost to true\n"
            f'    set value of attribute "AXMain" of window {index} to true\n'
            f'    set value of attribute "AXFocused" of window {index} to true\n'
            f'    perform action "AXRaise" of window {index}'
        )
        time.sleep(0.15)
        self.click_screen(
            float(native["x"]) + float(native["w"]) * 0.5,
            float(native["y"]) + float(native["h"]) * 0.45,
        )

    def start(self, command: str = "") -> None:
        self._launch(command, reset_daemon=True)

    def _launch(self, command: str, reset_daemon: bool) -> None:
        if not CONFIG_LUA.is_file():
            fail(f"missing WezTerm e2e config {CONFIG_LUA}")
        if reset_daemon:
            kill_htm_daemons()
        self.started_at = time.time() - 1.0
        env = os.environ.copy()
        wezterm_dir = str(self.wezterm_bin().parent)
        env["PATH"] = f"{self.htm.parent}:{wezterm_dir}:{env.get('PATH', '')}"
        env["WEZTERM_LOG"] = "info"
        STDERR_LOG.parent.mkdir(parents=True, exist_ok=True)
        self.stderr_file = open(STDERR_LOG, "w" if reset_daemon else "a")
        binary = self.wezterm_bin()
        command_argv = shlex.split(command.strip() or self.multiplexer_command())
        self.proc = subprocess.Popen(
            [
                str(binary),
                "--config-file",
                str(CONFIG_LUA),
                "start",
                "--always-new-process",
                "--",
                *command_argv,
            ],
            env=env,
            cwd=str(self.htm.parent),
            stdout=subprocess.DEVNULL,
            stderr=self.stderr_file,
            start_new_session=True,
        )
        wait_until(
            lambda: self.proc is not None and self.proc.poll() is None,
            5,
            description="WezTerm process start",
        )

        def window_ready() -> bool:
            if self.proc is not None and self.proc.poll() is not None:
                if self.stderr_file:
                    try:
                        self.stderr_file.flush()
                    except OSError:
                        pass
                fail_wezterm_crash("before opening a window")
            return self.window_count() > 0

        wait_until(window_ready, 25, description="WezTerm window")
        self.remember_gateway_windows()
        self.focus()

    def resize_front_native_window(self, width: int, height: int) -> None:
        self.focus_native_window()
        self.osascript_pid(
            "set frontmost to true\n"
            f"    set size of front window to {{{width}, {height}}}"
        )
        time.sleep(0.3)
        self.focus_native_window()

    def after_attach(self) -> None:
        if self.mux == "tmux":
            wait_until(
                lambda: self.tmux_has_session()
                and self.proc is not None
                and self.proc.poll() is None,
                15,
                description="WezTerm tmux -CC attach",
            )
            wait_until(
                lambda: self.window_count() >= 2,
                15,
                description="tmux -CC session in a separate WezTerm window",
            )
            print("OK: WezTerm attached to tmux -CC", flush=True)
        else:
            self.wait_log(
                lambda text: "list-panes" in text or "list-windows" in text,
                15,
                "WezTerm list-panes/list-windows",
            )
            wait_until(
                lambda: self.window_count() >= 2,
                15,
                description="HTM session in a separate WezTerm window",
            )
        expected_menu = (
            "** tmux mode started **\n\n"
            "Command Menu\n"
            "----------------------------\n"
            "esc    Detach cleanly.\n"
            "  X    Force-quit tmux mode.\n"
            "  L    Toggle logging.\n"
            "  C    Run tmux command."
        )
        wait_until(
            lambda: expected_menu in self.gateway_text(),
            10,
            description="iTerm2-compatible WezTerm tmux command menu",
        )
        self.remember_attached_gateway_window()
        time.sleep(1.0)
        if self.proc is not None and self.proc.poll() is not None:
            fail_wezterm_crash("after tmux -CC attach")
        self.gui_pid = None
        try:
            self.focus_native_window()
        except subprocess.CalledProcessError:
            pass

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            try:
                os.kill(self.proc.pid, signal.SIGTERM)
            except OSError:
                pass
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    os.kill(self.proc.pid, signal.SIGKILL)
                except OSError:
                    pass
                self.proc.wait(timeout=2)
        # Never kill the user's installed WezTerm. Only this invocation and
        # wezterm-gui children that were not already running.
        targets = []
        if self.proc:
            targets.append(self.proc.pid)
            targets.extend(descendant_pids(self.proc.pid))
        if self.gui_pid:
            targets.append(self.gui_pid)
        for pid in self._wezterm_gui_pids():
            if pid in self.preexisting_gui:
                continue
            targets.append(pid)
        for pid in dict.fromkeys(targets):
            if pid in self.preexisting_gui:
                continue
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass
        if self.stderr_file:
            try:
                self.stderr_file.close()
            except OSError:
                pass
            self.stderr_file = None

    def detach_client(self) -> None:
        self.remember_front_native()
        log_path = self.log_file if self.mux == "htm" else None
        log_before = (
            log_path.read_text(errors="replace")
            if log_path is not None and log_path.is_file()
            else ""
        )
        self.focus_gateway()
        self.key_code(53)
        if self.mux == "tmux":
            wait_until(
                lambda: self.tmux_has_session() and self.tmux_client_count() == 0,
                15,
                description="tmux session survived WezTerm detach",
            )
        else:
            watermark = len(log_before)

            def _detached() -> bool:
                if log_path is None or not log_path.is_file():
                    return False
                new = log_path.read_text(errors="replace")[watermark:]
                return any(
                    command.strip() in ("detach", "detach-client")
                    for command in control_commands(new)
                )

            wait_until(_detached, 15, description="htmd received WezTerm detach")
            wait_until(
                lambda: bool(pids_named("htmd")) and ipc_path().exists(),
                15,
                description="htmd survived WezTerm detach",
            )
        if self.mux == "htm":
            self._reattach_log_path = log_path
            self._reattach_log_watermark = (
                len(log_path.read_text(errors="replace"))
                if log_path is not None and log_path.is_file()
                else 0
            )
        print(f"OK: WezTerm detached without killing {self.mux}", flush=True)

    def reattach_client(self) -> None:
        self.focus_gateway()
        attach = (
            f"{self.tmux_bin} -L {self.tmux_socket} -f /dev/null "
            "-CC attach-session"
            if self.mux == "tmux"
            else str(self.htm)
        )
        previous = ""
        try:
            previous = subprocess.check_output(["pbpaste"], text=True)
        except (subprocess.CalledProcessError, FileNotFoundError):
            pass
        try:
            subprocess.run(["pbcopy"], input=attach, text=True, check=False)
            self.keystroke('"v"', "command down")
            self.key_code(36)
        finally:
            subprocess.run(["pbcopy"], input=previous, text=True, check=False)

        if self.mux == "tmux":
            wait_until(
                lambda: self.tmux_client_count() >= 1,
                20,
                description="tmux -CC client reattached",
            )
        else:
            log_path = getattr(self, "_reattach_log_path", None)
            watermark = getattr(self, "_reattach_log_watermark", 0)

            def _reattached() -> bool:
                if log_path is None or not log_path.is_file():
                    return False
                new = log_path.read_text(errors="replace")[watermark:]
                return "accepted, returned client_sock" in new and (
                    "control command: list-windows" in new
                    or "control command: list-panes" in new
                )

            wait_until(_reattached, 20, description="htm control client reattached")
        self.after_attach()
        self.sync_htm_window_recordings()
        print(f"OK: WezTerm reattached to {self.mux}", flush=True)

    def after_layout_suite(self) -> None:
        if self.mux == "tmux":
            if not self.tmux_has_session():
                fail("tmux -CC server exited during WezTerm layout suite")
            print("OK: WezTerm kept the tmux -CC session alive", flush=True)
            return
        text = self.log_text()
        if command_count(text, "list-windows") < 1 and "list-windows" not in text:
            fail("WezTerm did not send list-windows after control-mode attach")
        print("OK: WezTerm enumerated tmux windows", flush=True)

    def warn_leftovers(self) -> None:
        still = [
            pid
            for pid in self._wezterm_gui_pids()
            if pid not in self.preexisting_gui
        ]
        if still:
            print(f"WARN: leftover wezterm-gui pids {still}", flush=True)


def run_control_plane_checks(session: WezTermHtmSession) -> None:
    session.start(session.multiplexer_command())
    session.wait_init()
    session.after_attach()
    session.begin_htm_window_recording("control-plane")
    gateway_recorder = None
    if session.video_dir:
        gateway_title = (session.gateway_pane().get("title") or "").strip()
        gateways = [
            window
            for window in session.ax_windows()
            if (window.get("name") or "").strip() == gateway_title
        ]
        if gateways:
            window = gateways[0]
            region = (
                int(window["x"]),
                int(window["y"]),
                int(window["w"]),
                int(window["h"]),
            )
            gateway_recorder = ScreenRecorder(
                session.video_dir
                / f"wezterm-{session.mux}-control-plane-gateway.mov",
                region,
                f"WezTerm {session.mux} control gateway",
            )
            gateway_recorder.start()
    try:
        session.focus_gateway()
        session.keystroke('"l"')
        wait_until(
            lambda: "tmux logging enabled" in session.gateway_text(),
            10,
            description="WezTerm tmux protocol logging enabled",
        )

        session.keystroke('"c"')
        time.sleep(0.4)
        session.keystroke('"new-window"')
        session.key_code(36)
        session.wait_mux_window_count(2, timeout=15)
        wait_until(
            lambda: "> new-window" in session.gateway_text()
            and "< %begin" in session.gateway_text(),
            10,
            description="WezTerm displayed raw tmux protocol traffic",
        )
        session.sync_htm_window_recordings()
        print("OK: C ran new-window through the tmux command prompt", flush=True)

        session.focus_gateway()
        session.keystroke('"l"')
        wait_until(
            lambda: "tmux logging disabled" in session.gateway_text(),
            10,
            description="WezTerm tmux protocol logging disabled",
        )

        session.detach_client()
        session.reattach_client()

        session.focus_gateway()
        session.keystroke('"x"')
        if session.mux == "tmux":
            wait_until(
                lambda: session.tmux_has_session()
                and session.tmux_client_count() == 0,
                15,
                description="tmux server survived WezTerm force quit",
            )
        else:
            wait_until(
                lambda: bool(pids_named("htmd")) and ipc_path().exists(),
                15,
                description="htmd survived WezTerm force quit",
            )
        wait_until(
            lambda: session.window_count() == 1,
            15,
            description="WezTerm force quit closed native mux windows",
        )
        print(f"OK: X force-quit the {session.mux} client only", flush=True)
    finally:
        if gateway_recorder:
            gateway_recorder.stop(required=False)
        session.end_htm_window_recording()


def main() -> int:
    return run_emulator_main(sys.modules[__name__], default_suite="layout")


if __name__ == "__main__":
    raise SystemExit(main())
