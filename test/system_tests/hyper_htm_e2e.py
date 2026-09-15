#!/usr/bin/env python3
"""End-to-end tests for htm/htmd native Hyper integration.

Launches stock Hyper with the hyper-htm plugin and drives splits/tabs.
Protocol correctness is checked against htmd logs.

Skip (exit 77) when Hyper, the plugin, or htm/htmd is unavailable.
Not registered with default CTest; run this file directly (see AGENTS.md).
Expects Hyper's tmux -CC plugin (DCS 1000p from ``htm``).

Environment:
  HYPER_APP         Hyper.app (macOS) or Hyper.exe (Windows)
  HYPER_HTM_PLUGIN  Path to the hyper-htm plugin (default: ~/.hyper_plugins/local/hyper-htm)
  HTM_BIN           Path to the ``htm`` binary (overridden by --htm)
  HTMD_BIN          Path to the ``htmd`` binary (overridden by --htmd)
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

from htm_gui_e2e import (  # noqa: E402
    EXPECTED_TMUX_COMMAND_MENU,
    GuiTerminalSession,
    _is_gateway_title,
    _recording_window_key,
    _window_key,
    applescript_quote,
    fail,
    kill_htm_daemons,
    list_htmd_logs,
    newest_log,
    normalize_gateway_text,
    pids_named,
    process_is_running,
    read_text,
    run_emulator_main,
    run_osascript,
    skip,
    wait_until,
)


PLATFORMS = ("darwin", "win32")
NAME = "Hyper"


def is_hyper_app(app: Path) -> bool:
    if app.is_file() and app.name.casefold() == "hyper.exe":
        return True
    return (app / "Contents" / "MacOS" / "Hyper").is_file()


def candidate_hyper_apps() -> list[Path]:
    env = os.environ.get("HYPER_APP")
    paths: list[Path] = []
    if env:
        paths.append(Path(env))
    if os.name == "nt":
        local = Path(os.environ.get("LOCALAPPDATA", ""))
        paths.extend(
            [
                local / "Programs" / "Hyper" / "Hyper.exe",
                local / "Programs" / "hyper" / "Hyper.exe",
                local / "hyper" / "Hyper.exe",
                Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
                / "Hyper"
                / "Hyper.exe",
            ]
        )
        which = os.environ.get("PATH", "")
        for item in which.split(os.pathsep):
            candidate = Path(item) / "Hyper.exe"
            if candidate.is_file():
                paths.append(candidate)
    else:
        paths.append(Path("/Applications/Hyper.app"))
    seen = set()
    unique: list[Path] = []
    for path in paths:
        if not path:
            continue
        resolved = path.resolve() if path.exists() else path
        if resolved in seen:
            continue
        seen.add(resolved)
        unique.append(path)
    return unique


def find_hyper_app() -> Path:
    for path in candidate_hyper_apps():
        if is_hyper_app(path):
            return path.resolve() if path.exists() else path
    if os.name == "nt":
        skip("no Hyper.exe found; install Hyper or set HYPER_APP")
    skip("no Hyper.app found; install Hyper in /Applications or set HYPER_APP")


def find_hyper_plugin() -> Path:
    env = os.environ.get("HYPER_HTM_PLUGIN")
    if env:
        path = Path(env)
        if (path / "index.js").is_file() and (path / "htm-core.js").is_file():
            return path.resolve()
        fail(f"--plugin not found: {path}")
    installed = Path.home() / ".hyper_plugins" / "local" / "hyper-htm"
    if (installed / "index.js").is_file() and (installed / "htm-core.js").is_file():
        return installed.resolve()
    hyper_js = Path.home() / ".hyper.js"
    if hyper_js.is_file() and "hyper-htm" in hyper_js.read_text(errors="replace"):
        return hyper_js
    skip(
        "hyper-htm plugin is not installed; symlink it to "
        "~/.hyper_plugins/local/hyper-htm and add it to localPlugins in ~/.hyper.js"
    )


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--hyper-app")
    parser.add_argument("--plugin")


def apply_args(args: argparse.Namespace) -> None:
    if args.hyper_app:
        os.environ["HYPER_APP"] = args.hyper_app
    if args.plugin:
        os.environ["HYPER_HTM_PLUGIN"] = args.plugin
    plugin = find_hyper_plugin()
    print(f"Using plugin={plugin}", flush=True)


def open_session(htm: Path, htmd: Path, args: argparse.Namespace):
    app = find_hyper_app()
    if os.name == "nt":
        return WindowsHyperHtmSession(app, htm, htmd)
    return HyperHtmSession(app, htm, htmd)


if os.name == "nt":
    import ctypes
    from ctypes import wintypes
    from windows_terminal_htm_e2e import (  # noqa: E402
        KEYEVENTF_KEYUP,
        VK_CONTROL,
        VK_MENU,
        VK_RETURN,
        VK_SHIFT,
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
    VK_PRIOR = 0x21
    VK_NEXT = 0x22
    VK_LWIN = 0x5B

    def release_modifiers() -> None:
        for vk in (VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN):
            send_key(vk, KEYEVENTF_KEYUP)

    def type_vk_text(text: str) -> None:
        release_modifiers()
        for char in text:
            packed = int(user32.VkKeyScanW(ord(char)))
            if packed in (-1, 0xFFFF, 0xFFFFFFFF):
                type_text(char)
                time.sleep(0.015)
                continue
            vk = packed & 0xFF
            mods: list[int] = []
            if packed & 0x200:
                mods.append(VK_CONTROL)
            if packed & 0x100:
                mods.append(VK_SHIFT)
            shortcut(*mods, vk)
            release_modifiers()
            time.sleep(0.015)

    class WindowsHyperHtmSession(GuiTerminalSession):
        name = NAME
        supports_detach = False
        supports_native_resize = False

        def __init__(self, app: Path, htm: Path, htmd: Path):
            super().__init__(htm, htmd)
            self.app = app
            self.proc: Optional[subprocess.Popen] = None
            self.hwnd = 0
            self.preexisting = set(processes_named("Hyper.exe"))

        def _owned_pids(self) -> set[int]:
            return set(processes_named("Hyper.exe")) - self.preexisting

        def _owned_hwnds(self) -> list[int]:
            pids = self._owned_pids() or set(processes_named("Hyper.exe"))
            return [hwnd for hwnd, pid, _title in windows() if pid in pids]

        def _remember_window(self) -> bool:
            hwnds = self._owned_hwnds()
            if not hwnds:
                return False
            self.hwnd = hwnds[0]
            return True

        def wait_init(self, timeout: float = 25.0) -> str:
            def ready() -> bool:
                path = newest_log(list_htmd_logs(), self.started_at)
                text = read_text(path)
                if path and "control command:" in text:
                    self.log_file = path
                    return True
                return False

            wait_until(
                ready,
                timeout,
                description="htmd control command after Hyper attach",
            )
            return read_text(self.log_file)

        def tab_count(self) -> int:
            return max(len(self._owned_hwnds()), 1 if self.hwnd else 0)

        def window_count(self) -> int:
            return self.tab_count()

        def ax_windows(self) -> list[dict]:
            rect = wintypes.RECT()
            out: list[dict] = []
            for index, hwnd in enumerate(self._owned_hwnds(), start=1):
                if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
                    continue
                out.append(
                    {
                        "index": index,
                        "name": f"hwnd-{hwnd}",
                        "x": float(rect.left),
                        "y": float(rect.top),
                        "w": float(max(0, rect.right - rect.left)),
                        "h": float(max(0, rect.bottom - rect.top)),
                        "id": int(hwnd),
                        "hwnd": int(hwnd),
                    }
                )
            return out

        def launched_windows(self) -> list[dict]:
            return [win for win in self.ax_windows() if win["w"] >= 64 and win["h"] >= 64]

        def _raise_ax_window(self, win: dict) -> None:
            hwnd = int(win.get("hwnd") or win.get("id") or 0)
            if not hwnd:
                fail(f"Hyper window missing hwnd: {win!r}")
            focus_window(hwnd)
            time.sleep(0.2)

        def focus(self, *, prefer_front: bool = True) -> None:
            owned = self._owned_hwnds()
            if not owned:
                return
            owned_set = set(owned)
            foreground = int(user32.GetForegroundWindow() or 0)
            if prefer_front and foreground in owned_set:
                self.hwnd = foreground
                return
            if self.hwnd not in owned_set:
                self.hwnd = owned[0]
            if self.hwnd:
                focus_window(self.hwnd)

        def keystroke(self, keys: str, using: str = "") -> None:
            value = keys.strip('"')
            self.focus()
            if "command" in using:
                action = (value, "shift" in using)
                mapping = {
                    ("d", False): (VK_CONTROL, VK_SHIFT, ord("D")),
                    ("d", True): (VK_CONTROL, VK_SHIFT, ord("E")),
                    ("t", False): (VK_CONTROL, VK_SHIFT, ord("T")),
                    ("n", False): (VK_CONTROL, VK_SHIFT, ord("N")),
                    ("w", False): (VK_CONTROL, VK_SHIFT, ord("W")),
                    ("[", False): (VK_CONTROL, VK_NEXT),
                    ("]", False): (VK_CONTROL, VK_PRIOR),
                    ("[", True): (VK_CONTROL, VK_SHIFT, VK_TAB),
                }
                chord = mapping.get(action)
                if chord is None:
                    fail(f"unsupported Hyper Windows action: {keys} {using}")
                if value == "w":
                    time.sleep(1.2)
                shortcut(*chord)
                release_modifiers()
                time.sleep(0.7)
                if value == "t" and not action[1]:
                    hwnds = self._owned_hwnds()
                    if hwnds:
                        self.hwnd = hwnds[0]
                        focus_window(self.hwnd)
                return
            self.focus()
            type_vk_text(value)

        def key_code(self, code: int, using: str = "") -> None:
            if code != 36:
                fail(f"unsupported Hyper key code: {code}")
            self.focus()
            shortcut(VK_RETURN)
            time.sleep(0.12)

        def previous_pane(self) -> None:
            self.focus()
            shortcut(VK_CONTROL, VK_NEXT)
            release_modifiers()
            time.sleep(0.8)

        def next_pane(self) -> None:
            self.focus()
            shortcut(VK_CONTROL, VK_PRIOR)
            release_modifiers()
            time.sleep(0.8)

        def start(self, command: str = "") -> None:
            kill_htm_daemons()
            for pid in list(processes_named("Hyper.exe")):
                terminate_pid(pid)
            wait_until(
                lambda: not processes_named("Hyper.exe"),
                10,
                description="Hyper exit before relaunch",
            )
            self.preexisting = set()
            self.started_at = time.time() - 1.0
            env = os.environ.copy()
            env["PATH"] = f"{self.htm.parent}{os.pathsep}{env.get('PATH', '')}"
            self.proc = subprocess.Popen(
                [str(self.app)],
                env=env,
                cwd=str(self.htm.parent),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
                | getattr(subprocess, "DETACHED_PROCESS", 0),
            )
            wait_until(self._remember_window, 20, description="Hyper window")
            self.focus()
            time.sleep(1.2)
            launch = command.strip() if command else f"{self.htm} -x"
            type_vk_text(launch)
            shortcut(VK_RETURN)

        def after_first_split(self) -> None:
            time.sleep(1.0)
            release_modifiers()

        def after_attach(self) -> None:
            time.sleep(2.0)
            self.focus()
            time.sleep(0.3)

        def stop(self) -> None:
            if self.proc and self.proc.poll() is None:
                try:
                    self.proc.terminate()
                except OSError:
                    pass
            for pid in self._owned_pids() or processes_named("Hyper.exe"):
                terminate_pid(pid)
            kill_htm_daemons()


class HyperHtmSession(GuiTerminalSession):
    name = "Hyper"
    ax_process_name = "Hyper"
    supports_detach = True

    def __init__(self, app: Path, htm: Path, htmd: Path):
        super().__init__(htm, htmd)
        self.app = app
        self.was_running = False

    def osascript_hyper(self, body: str) -> str:
        script = f'''
tell application "System Events"
  tell process "Hyper"
    {body}
  end tell
end tell
'''
        return run_osascript(script)

    def focus(self) -> None:
        run_osascript('tell application "Hyper" to activate')
        try:
            self.osascript_hyper("set frontmost to true")
        except subprocess.CalledProcessError:
            pass
        time.sleep(0.15)

    def keystroke(self, keys: str, using: str = "") -> None:
        using_clause = f" using {using}" if using else ""
        self.focus()
        run_osascript(
            f'tell application "System Events" to keystroke {keys}{using_clause}'
        )
        time.sleep(0.08)

    def key_code(self, code: int, using: str = "") -> None:
        using_clause = f" using {using}" if using else ""
        self.focus()
        run_osascript(
            f"tell application \"System Events\" to key code {code}{using_clause}"
        )
        time.sleep(0.08)

    def window_count(self) -> int:
        try:
            out = self.osascript_hyper("get count of windows").strip()
            return int(out)
        except (ValueError, subprocess.CalledProcessError):
            return 0

    def tab_count(self) -> int:
        # Prefer AX tabs in the front window when Hyper groups tmux windows
        # as tabs (Cmd+T affinity); fall back to OS window count.
        try:
            out = self.osascript_hyper(
                'count of (every UI element of window 1 whose role is "AXRadioButton")'
            ).strip()
            n = int(out)
            if n > 0:
                return n
        except (ValueError, subprocess.CalledProcessError):
            pass
        return self.window_count()

    def start(self, command: str = "") -> None:
        self.was_running = process_is_running("Hyper")
        kill_htm_daemons()
        try:
            run_osascript('tell application "Hyper" to quit')
        except subprocess.CalledProcessError:
            pass
        time.sleep(1.5)
        self.started_at = time.time() - 1.0
        subprocess.run(["open", "-na", str(self.app)], check=True)
        wait_until(
            lambda: self.window_count() > 0,
            15,
            description="Hyper window",
        )
        self.focus()
        time.sleep(0.8)
        self.remember_gateway_windows()
        launch = command.strip() if command else f"{self.htm} -x"
        self.keystroke(applescript_quote(launch))
        self.key_code(36)

    def multiplexer_command(self) -> str:
        # Keep a login shell after Esc detach so control-plane can re-attach.
        # Avoid nested double-quotes — AppleScript keystroke quoting breaks.
        command = super().multiplexer_command()
        return f"{command}; exec /bin/zsh -l"

    def focus_gateway(self) -> None:
        """Raise the control-plane Hyper window (not a mux follower)."""
        self._prefer_gateway_input = True
        self.focus()
        gateways = [
            w
            for w in self.ax_windows()
            if _window_key(w) in self._gateway_keys
            or _is_gateway_title(w.get("name") or "")
        ]
        if not gateways:
            # Fallback: any window that is not a launched mux surface.
            launched_keys = {_recording_window_key(w) for w in self.launched_windows()}
            gateways = [
                w
                for w in self.ax_windows()
                if _recording_window_key(w) not in launched_keys
            ]
        if not gateways:
            gateways = self.ax_windows()[:1]
        if gateways:
            win = gateways[0]
            self._raise_ax_window(win)
            x = int(float(win["x"]) + float(win["w"]) * 0.5)
            y = int(float(win["y"]) + float(win["h"]) * 0.55)
            try:
                self.osascript_hyper(f"click at {{{x}, {y}}}")
            except subprocess.CalledProcessError:
                self.click_screen(x, y)
        time.sleep(0.3)

    def _focused_terminal_text(self) -> str:
        """Read Hyper's rendered active xterm buffer via select-all + copy."""
        previous = ""
        try:
            previous = subprocess.check_output(["pbpaste"], text=True)
        except (subprocess.CalledProcessError, FileNotFoundError):
            pass
        try:
            self.keystroke('"a"', "command down")
            time.sleep(0.1)
            self.keystroke('"c"', "command down")
            time.sleep(0.15)
            try:
                return subprocess.check_output(["pbpaste"], text=True)
            except (subprocess.CalledProcessError, FileNotFoundError):
                return ""
        finally:
            subprocess.run(["pbcopy"], input=previous, text=True, check=False)

    def rendered_terminal_text(self) -> Optional[str]:
        """Read the focused mux pane, not tmux/htm's backend capture."""
        self.focus()
        time.sleep(0.1)
        return self._focused_terminal_text()

    def gateway_text(self) -> str:
        """Read gateway visible text via select-all + clipboard."""
        self.focus_gateway()
        return self._focused_terminal_text()

    def after_attach(self) -> None:
        self._prefer_gateway_input = False
        time.sleep(2.0)
        self.focus()
        wait_until(
            lambda: (
                len(self.launched_windows()) >= 1
                or self.tab_count() > 1
                or "tmux mode started" in self.gateway_text()
            ),
            15,
            description="Hyper native mux OS window after attach",
        )
        self._split_axis = "x"
        self._pane_click_x = 0.72
        self._pane_click_y = 0.55
        if self.launched_windows():
            self.focus_native_window()
        time.sleep(0.4)

    def reattach_client(self) -> None:
        """Type attach into the gateway shell (Hyper paste is unreliable)."""
        # Esc detach currently tears down every Hyper window (plugin closes
        # followers and the gateway PTY can vanish). Open a fresh window.
        if self.window_count() == 0:
            self._prefer_gateway_input = True
            run_osascript('tell application "Hyper" to activate')
            time.sleep(0.3)
            run_osascript(
                'tell application "System Events" to '
                'keystroke "n" using command down'
            )
            wait_until(
                lambda: self.window_count() >= 1,
                15,
                description="Hyper window after detach reopen",
            )
            time.sleep(1.5)
            self.remember_gateway_windows()
        else:
            wait_until(
                lambda: self.window_count() <= 1,
                15,
                description="Hyper mux windows closed after detach",
            )
            time.sleep(0.5)
        self.focus_gateway()
        time.sleep(0.4)
        attach = (
            f"{self.tmux_bin} -L {self.tmux_socket} -f /dev/null "
            "-CC attach-session"
            if self.mux == "tmux"
            else str(self.htm)
        )
        self.keystroke('"c"', "control down")
        time.sleep(0.15)
        self.keystroke('"u"', "control down")
        time.sleep(0.15)
        previous = ""
        try:
            previous = subprocess.check_output(["pbpaste"], text=True)
        except (subprocess.CalledProcessError, FileNotFoundError):
            pass
        try:
            subprocess.run(["pbcopy"], input=attach + "\n", text=True, check=False)
            self.keystroke('"v"', "command down")
            time.sleep(0.2)
            # Enter in case paste did not include a trailing newline that the
            # shell executes immediately.
            self.key_code(36)
        finally:
            subprocess.run(["pbcopy"], input=previous, text=True, check=False)
        time.sleep(1.0)
        if self.mux == "tmux":
            wait_until(
                lambda: self.tmux_client_count() >= 1,
                20,
                description=f"{self.name} tmux -CC client reattached",
            )
        else:
            # After Esc detach Hyper may rematerialize mux panes as tabs on
            # the reopened gateway window (launched_windows stays empty).
            wait_until(
                lambda: (
                    len(self.launched_windows()) >= 1
                    or self.tab_count() > 1
                    or "tmux mode started" in self.gateway_text()
                ),
                30,
                description=f"{self.name} htm control client reattached",
            )
            # after_attach expects a launched mux window; if panes are tabs on
            # the gateway, synthesize a native surface via New Tmux Window.
            if not self.launched_windows() and self.tab_count() > 1:
                print(
                    "WARN: htm reattach rematerialized as gateway tabs; "
                    "opening a New Tmux Window for a native mux surface",
                    flush=True,
                )
                self._prefer_gateway_input = False
                self.focus_gateway()
                # Force a separate OS window for subsequent force-quit checks.
                run_osascript(
                    'tell application "System Events" to '
                    'keystroke "n" using command down'
                )
                wait_until(
                    lambda: self.window_count() >= 2
                    or len(self.launched_windows()) >= 1,
                    20,
                    description="native mux window after htm reattach",
                )
        self.after_attach()
        self.sync_htm_window_recordings()
        print(f"OK: {self.name} reattached to {self.mux}", flush=True)

    def after_first_split(self) -> None:
        self.after_split(vertical=False)

    def after_split(self, *, vertical: bool = False) -> None:
        time.sleep(0.5)
        if vertical:
            self._split_axis = "y"
            self._pane_click_x = 0.5
            self._pane_click_y = 0.72
        else:
            self._split_axis = "x"
            self._pane_click_x = 0.72
            self._pane_click_y = 0.55
        self._raise_mux_for_input(click=True)
        time.sleep(0.3)

    def focus_newest_tab(self) -> None:
        """Click the last AX tab in the front Hyper window (Cmd+T target)."""
        try:
            self.osascript_hyper(
                """
set tabs to every UI element of window 1 whose role is "AXRadioButton"
if (count of tabs) > 0 then
  click (last item of tabs)
end if
"""
            )
        except subprocess.CalledProcessError:
            pass
        time.sleep(0.25)

    def after_new_window(self) -> None:
        time.sleep(0.5)
        self.focus_newest_tab()
        # New tab starts with a single pane; next split sets axis again.
        self._split_axis = "x"
        self._pane_click_x = 0.5
        self._pane_click_y = 0.55
        self._raise_mux_for_input(click=True)
        time.sleep(0.35)

    def previous_pane(self) -> None:
        # Click the opposite half — Hyper's Cmd+[ is unreliable under e2e
        # focus churn, and a fixed click would undo keyboard pane focus.
        if getattr(self, "_split_axis", "x") == "y":
            self._pane_click_y = 0.28 if self._pane_click_y >= 0.5 else 0.72
        else:
            self._pane_click_x = 0.28 if self._pane_click_x >= 0.5 else 0.72
        self._raise_mux_for_input(click=True)
        time.sleep(0.35)

    def next_pane(self) -> None:
        if getattr(self, "_split_axis", "x") == "y":
            self._pane_click_y = 0.72 if self._pane_click_y < 0.5 else 0.28
        else:
            self._pane_click_x = 0.72 if self._pane_click_x < 0.5 else 0.28
        self._raise_mux_for_input(click=True)
        time.sleep(0.35)

    def _raise_mux_for_input(self, *, click: bool = True) -> None:
        """Keep keystrokes on a mux OS window, not the control plate."""
        if getattr(self, "_prefer_gateway_input", False):
            self.focus()
            time.sleep(0.05)
            return
        launched = self.launched_windows()
        if not launched:
            self.focus()
            return
        target = self._resolve_front_native(launched)
        self._front_native = target
        wid = int(target.get("id") or 0)
        finder = (
            f"(first window whose id is {wid})"
            if wid
            else f"window {int(target['index'])}"
        )
        if not click:
            self.focus()
            try:
                run_osascript(
                    f'''
tell application "System Events"
  tell process "Hyper"
    if frontmost is false then set frontmost to true
    set w to {finder}
    try
      perform action "AXRaise" of w
    end try
    try
      set index of w to 1
    end try
  end tell
end tell
'''
                )
            except (subprocess.CalledProcessError, SystemExit, ValueError, KeyError):
                pass
            time.sleep(0.08)
            return
        self.focus()
        x = float(target["x"]) + float(target["w"]) * float(
            getattr(self, "_pane_click_x", 0.72)
        )
        y = float(target["y"]) + float(target["h"]) * float(
            getattr(self, "_pane_click_y", 0.55)
        )
        try:
            run_osascript(
                f'''
tell application "System Events"
  tell process "Hyper"
    set frontmost to true
    set w to {finder}
    try
      perform action "AXRaise" of w
    end try
    try
      set index of w to 1
    end try
    click at {{{int(x)}, {int(y)}}}
  end tell
end tell
'''
            )
        except subprocess.CalledProcessError:
            self.click_screen(x, y)
        time.sleep(0.2)

    def _resolve_front_native(self, launched: list) -> dict:
        """Pick ``_front_native`` by stable id/position, not stale AX index."""
        target = self._front_native
        if target is None:
            return launched[0]
        tid = int(target.get("id") or 0)
        if tid:
            for w in launched:
                if int(w.get("id") or 0) == tid:
                    return w
        tkey = _recording_window_key(target)
        for w in launched:
            if _recording_window_key(w) == tkey:
                return w
        # Fall back to position proximity (Electron ids can churn).
        try:
            tx, ty = int(target["x"]), int(target["y"])
        except (KeyError, TypeError, ValueError):
            return launched[0]
        for w in launched:
            if abs(int(w["x"]) - tx) <= 40 and abs(int(w["y"]) - ty) <= 40:
                return w
        return launched[0]

    def keystroke(self, keys: str, using: str = "") -> None:
        using_clause = f" using {using}" if using else ""
        # Modifier chords must not re-click; that undoes Cmd+[ pane focus.
        self._raise_mux_for_input(click=not bool(using))
        run_osascript(
            f'tell application "System Events" to keystroke {keys}{using_clause}'
        )
        time.sleep(0.12)

    def key_code(self, code: int, using: str = "") -> None:
        using_clause = f" using {using}" if using else ""
        # Never re-click before Return: a click between the command text and
        # Enter left followers with ``echo MARKER`` on the line (wait_visible
        # still matched the typed substring) and automatic-rename never saw
        # ``sleep``.
        self._raise_mux_for_input(click=False)
        if code == 36 and not using:
            run_osascript(
                'tell application "System Events" to keystroke return'
            )
        else:
            run_osascript(
                f"tell application \"System Events\" to key code {code}{using_clause}"
            )
        time.sleep(0.12)

    def submit_text(self, command: str) -> None:
        """Type a command and Return without an intervening mouse click."""
        self._raise_mux_for_input(click=True)
        run_osascript(
            "tell application \"System Events\" to "
            f"keystroke {applescript_quote(command)} & return"
        )
        time.sleep(0.2)

    def stop(self) -> None:
        try:
            self.keystroke('"w"', "{command down, shift down}")
            time.sleep(0.3)
        except subprocess.CalledProcessError:
            pass
        if not self.was_running:
            try:
                run_osascript('tell application "Hyper" to quit')
            except subprocess.CalledProcessError:
                pass
        kill_htm_daemons()


def main() -> int:
    return run_emulator_main(sys.modules[__name__], default_suite="all")


if __name__ == "__main__":
    raise SystemExit(main())
