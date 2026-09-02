#!/usr/bin/env python3
"""End-to-end tests for htm/htmd native iTerm2 integration.

Launches stock iTerm2 (``/Applications/iTerm.app``) with ``-suite`` so the
user's installed prefs and windows are left alone. Protocol correctness is
checked against htmd logs; native tabs/panes are checked via Accessibility.

Skip (exit 77) when iTerm2, htm/htmd, or Accessibility is unavailable.
Not registered with default CTest; run this file directly (see AGENTS.md).
Expects iTerm2's native tmux -CC integration (DCS 1000p from ``htm``).

Environment:
  ITERM2_APP   Path to iTerm2.app (default: /Applications/iTerm.app)
  HTM_BIN      Path to the ``htm`` binary (overridden by --htm)
  HTMD_BIN     Path to the ``htmd`` binary (overridden by --htmd)
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

from htm_gui_e2e import (  # noqa: E402
    GuiTerminalSession,
    commands_containing,
    control_commands,
    fail,
    ipc_path,
    pids_named,
    run_emulator_main,
    run_osascript,
    skip,
    wait_until,
)

SUITE = "EternalTerminalHtmE2E"

PROTOCOL_LEAK_RE = re.compile(
    r"%(?:output|extended-output|layout-change|session-changed|"
    r"sessions-changed|window-add|window-close|window-pane-changed|"
    r"session-window-changed|begin|end|exit)\b"
)
CAPTURE_DIR = Path("/tmp/iterm2-htm-e2e")


def is_iterm_app(app: Path) -> bool:
    return (app / "Contents" / "MacOS" / "iTerm2").is_file()


def candidate_iterm_apps() -> list[Path]:
    env = os.environ.get("ITERM2_APP")
    paths: list[Path] = []
    if env:
        paths.append(Path(env))
    paths.extend(
        [
            Path("/Applications/iTerm.app"),
            Path("/Applications/iTerm2.app"),
        ]
    )
    seen = set()
    unique = []
    for path in paths:
        resolved = path.resolve() if path.exists() else path
        if resolved in seen:
            continue
        seen.add(resolved)
        unique.append(path)
    return unique


def find_iterm_app() -> Path:
    for path in candidate_iterm_apps():
        if path.is_dir() and is_iterm_app(path):
            return path
    skip(
        "no iTerm2.app found; install iTerm2 in /Applications or set ITERM2_APP"
    )


def configure_suite_defaults() -> None:
    subprocess.run(
        ["defaults", "delete", SUITE, "GlobalKeyMap"],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    bools = {
        "EnableAPIServer": True,
        "PromptOnQuit": False,
        "OnlyWhenMoreTabs": False,
        "OpenArrangementAtStartup": False,
        "OpenNoWindowsAtStartup": False,
        "SUEnableAutomaticChecks": False,
        "NoSyncNeverRemindPrefsChangesAgain": True,
        "HideTab": False,
    }
    for key, enabled in bools.items():
        subprocess.run(
            ["defaults", "write", SUITE, key, "-bool", "true" if enabled else "false"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


NAME = "iTerm2"


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--iterm2-app")


def apply_args(args: argparse.Namespace) -> None:
    if args.iterm2_app:
        os.environ["ITERM2_APP"] = args.iterm2_app


def open_session(htm: Path, htmd: Path, args: argparse.Namespace) -> "ITermHtmSession":
    return ITermHtmSession(find_iterm_app(), htm, htmd)


class ITermHtmSession(GuiTerminalSession):
    name = "iTerm2"

    def __init__(self, app: Path, htm: Path, htmd: Path):
        super().__init__(htm, htmd)
        self.app = app
        self.proc: Optional[subprocess.Popen] = None
        self.preexisting_iterm = set(self._iterm_pids())
        self.iterm2_set_ids: list[str] = []

    def _iterm_pids(self) -> list[int]:
        pids = []
        try:
            out = subprocess.check_output(["pgrep", "-f", "iTerm2"], text=True)
        except subprocess.CalledProcessError:
            return []
        for line in out.split():
            try:
                pids.append(int(line))
            except ValueError:
                continue
        return pids

    def osascript_pid(self, body: str) -> str:
        script = f'''
tell application "System Events"
  tell (first process whose unix id is {self.pid})
    {body}
  end tell
end tell
'''
        return run_osascript(script)

    @property
    def pid(self) -> int:
        if not self.proc or self.proc.poll() is not None:
            fail("iTerm2 process is not running")
        return self.proc.pid

    def focus(self) -> None:
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

    def window_count(self) -> int:
        try:
            out = self.osascript_pid("get count of windows").strip()
            return int(out)
        except (ValueError, subprocess.CalledProcessError):
            return 0

    def tab_count(self) -> int:
        """Best-effort native tab count for the front window."""
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

    def session_splitter_count(self) -> int:
        try:
            out = self.osascript_pid(
                'count of (every splitter group of window 1)'
            ).strip()
            return int(out)
        except (ValueError, subprocess.CalledProcessError):
            return 0

    def select_first_tab(self) -> None:
        """Focus the gateway tab (first) so Esc/x reach the HTM command menu."""
        try:
            self.osascript_pid(
                "set frontmost to true\n    click radio button 1 of tab group 1 of window 1"
            )
        except subprocess.CalledProcessError:
            self.keystroke('"1"', "command down")
        time.sleep(0.35)

    def previous_pane(self) -> None:
        """Select the previous split pane (Window > Split Pane > Select Split Pane)."""
        try:
            self.osascript_pid(
                "set frontmost to true\n"
                '    click menu item "Previous Pane" of menu "Select Split Pane" '
                'of menu item "Select Split Pane" of menu "Split Pane" '
                'of menu item "Split Pane" of menu "Window" of menu bar 1'
            )
        except subprocess.CalledProcessError:
            self.keystroke('"["', "command down")
        time.sleep(0.35)

    def next_pane(self) -> None:
        try:
            self.osascript_pid(
                "set frontmost to true\n"
                '    click menu item "Next Pane" of menu "Select Split Pane" '
                'of menu item "Select Split Pane" of menu "Split Pane" '
                'of menu item "Split Pane" of menu "Window" of menu bar 1'
            )
        except subprocess.CalledProcessError:
            self.keystroke('"]"', "command down")
        time.sleep(0.35)

    def previous_tab(self) -> None:
        self.keystroke('"["', "{command down, shift down}")
        time.sleep(0.4)

    def click_menu(self, menu_bar_item: str, *path: str) -> None:
        """Click a nested menu item under ``menu_bar_item`` on the menu bar."""
        if not path:
            fail("click_menu requires at least one menu item")
        body = f'click menu item "{path[-1]}"'
        for name in reversed(path[:-1]):
            body += f' of menu "{name}" of menu item "{name}"'
        body += f' of menu "{menu_bar_item}" of menu bar 1'
        self.osascript_pid(f"set frontmost to true\n    {body}")
        time.sleep(0.35)

    def window_frame(self) -> tuple[float, float, float, float]:
        def parse_pair(raw: str) -> tuple[float, float]:
            parts = raw.replace("{", "").replace("}", "").split(",")
            return float(parts[0].strip()), float(parts[1].strip())

        x, y = parse_pair(self.osascript_pid("get position of window 1"))
        width, height = parse_pair(self.osascript_pid("get size of window 1"))
        return x, y, width, height

    def click_screen(self, x: float, y: float) -> None:
        """Left-click global screen coordinates (origin top-left) via CoreGraphics."""
        import ctypes
        import ctypes.util

        libname = ctypes.util.find_library("ApplicationServices") or ctypes.util.find_library(
            "CoreGraphics"
        )
        if not libname:
            fail("CoreGraphics is not available for pane clicks")
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

        kCGHIDEventTap = 0
        kCGEventMouseMoved = 5
        kCGEventLeftMouseDown = 1
        kCGEventLeftMouseUp = 2
        kCGMouseButtonLeft = 0
        point = CGPoint(float(x), float(y))
        moved = cg.CGEventCreateMouseEvent(None, kCGEventMouseMoved, point, 0)
        if moved:
            cg.CGEventPost(kCGHIDEventTap, moved)
            cg.CFRelease(moved)
        time.sleep(0.05)
        down = cg.CGEventCreateMouseEvent(None, kCGEventLeftMouseDown, point, kCGMouseButtonLeft)
        if not down:
            fail(f"CGEventCreateMouseEvent failed at {int(x)},{int(y)}")
        cg.CGEventPost(kCGHIDEventTap, down)
        cg.CFRelease(down)
        time.sleep(0.05)
        up = cg.CGEventCreateMouseEvent(None, kCGEventLeftMouseUp, point, kCGMouseButtonLeft)
        cg.CGEventPost(kCGHIDEventTap, up)
        cg.CFRelease(up)
        time.sleep(0.2)

    def pane_points(self) -> tuple[tuple[float, float], tuple[float, float]]:
        """Approximate centers of the left and right halves of window 1."""
        x, y, width, height = self.window_frame()
        cy = y + height * 0.62
        left = (x + width * 0.22, cy)
        right = (x + width * 0.78, cy)
        return left, right

    def click_pane_half(self, side: str) -> None:
        left, right = self.pane_points()
        pt = left if side == "left" else right
        self.focus()
        self.click_screen(pt[0], pt[1])

    def screenshot(self, label: str) -> Path:
        """Capture window 1 of the suite iTerm2 instance."""
        CAPTURE_DIR.mkdir(parents=True, exist_ok=True)
        path = CAPTURE_DIR / f"{label}.png"
        self.focus()
        time.sleep(0.2)
        x, y, width, height = self.window_frame()
        region = f"{int(x)},{int(y)},{int(width)},{int(height)}"
        subprocess.check_call(
            ["screencapture", "-R", region, "-x", str(path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return path

    def visible_contents(self) -> str:
        """Copy the focused session buffer (suite iTerm is not Apple-scriptable)."""
        self.focus()
        previous = ""
        try:
            previous = subprocess.check_output(["pbpaste"], text=True)
        except (subprocess.CalledProcessError, FileNotFoundError):
            previous = ""
        try:
            self.keystroke('"a"', "command down")
            time.sleep(0.2)
            self.keystroke('"c"', "command down")
            time.sleep(0.25)
            try:
                return subprocess.check_output(["pbpaste"], text=True)
            except (subprocess.CalledProcessError, FileNotFoundError):
                return ""
        finally:
            subprocess.run(
                ["pbcopy"],
                input=previous,
                text=True,
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

    def dump_visible(self, label: str, require: Optional[str] = None) -> str:
        """Screenshot + dump session text; fail on leaked tmux -CC protocol."""
        img = self.screenshot(label)
        text = self.visible_contents()
        dump = CAPTURE_DIR / f"{label}.txt"
        dump.write_text(text)
        print(f"visible[{label}] screenshot={img} chars={len(text)}", flush=True)
        for line in text.splitlines()[:40]:
            if line.strip():
                print(f"  | {line[:160]}", flush=True)
        leak = PROTOCOL_LEAK_RE.search(text)
        if leak:
            fail(
                f"control-mode {leak.group(0)!r} leaked into visible iTerm2 text "
                f"at {label}:\n{text[:2000]}"
            )
        if require and require not in text:
            fail(
                f"expected {require!r} in visible iTerm2 text at {label} "
                f"(screenshot={img}):\n{text[:2000]}"
            )
        return text

    def start(self, command: str) -> None:
        configure_suite_defaults()
        self.started_at = time.time() - 1.0
        env = os.environ.copy()
        env["PATH"] = f"{self.htm.parent}:{env.get('PATH', '')}"
        env["IT2_SUITE"] = SUITE
        binary = self.app / "Contents" / "MacOS" / "iTerm2"
        command = f"{self.htm} -x"
        self.proc = subprocess.Popen(
            [
                str(binary),
                "-suite",
                SUITE,
                f"--command={command}",
            ],
            env=env,
            cwd=str(self.htm.parent),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        wait_until(
            lambda: self.proc is not None and self.proc.poll() is None,
            5,
            description="iTerm2 process start",
        )
        wait_until(lambda: self.window_count() > 0, 25, description="iTerm2 window")
        self.focus()

    def _pid_command(self, pid: int) -> str:
        try:
            return subprocess.check_output(
                ["ps", "-p", str(pid), "-o", "command="],
                text=True,
                stderr=subprocess.DEVNULL,
            )
        except subprocess.CalledProcessError:
            return ""

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
        # Never kill the user's installed iTerm2. Only the suite instance and
        # processes whose command line names this isolated defaults suite.
        for pid in self._iterm_pids():
            if pid in self.preexisting_iterm:
                continue
            if self.proc and pid == self.proc.pid:
                continue
            cmdline = self._pid_command(pid)
            if SUITE in cmdline:
                try:
                    os.kill(pid, signal.SIGTERM)
                except OSError:
                    pass

    def after_attach(self) -> None:
        tabs_after_init = self.tab_count()
        if tabs_after_init < 2:
            print(
                f"WARN: AX tab count after control-mode attach was {tabs_after_init}; "
                "continuing with protocol assertions",
                flush=True,
            )
        else:
            print(f"OK: control-mode created native tabs ({tabs_after_init})", flush=True)

        attach_log = self.wait_log(
            lambda text: (
                any(
                    cmd.startswith("set") and "@iterm2_id" in cmd
                    for cmd in control_commands(text)
                )
                and any(
                    cmd.startswith("show") and "@iterm2_id" in cmd
                    for cmd in control_commands(text)
                )
                and (
                    commands_containing(text, "@affinities")
                    or commands_containing(text, "@uservars")
                    or commands_containing(text, "show-options")
                )
            ),
            20,
            "attach @iterm2_id/@affinities/@uservars",
        )
        self.iterm2_set_ids = [
            cmd
            for cmd in commands_containing(attach_log, "@iterm2_id")
            if cmd.startswith("set")
        ]
        print("OK: attach stored and queried @ user options", flush=True)
        self.dump_visible("01-after-attach")

    def after_first_split(self) -> None:
        try:
            self.click_menu("Session", "Move Session", "Move Session to Split Pane")
        except subprocess.CalledProcessError as exc:
            fail(f"Move Session to Split Pane menu failed: {exc.output or exc}")
        time.sleep(0.5)
        self.click_pane_half("left")
        self.wait_log(
            lambda text: any(cmd.startswith("move-pane") for cmd in control_commands(text)),
            20,
            "move-pane after Move Session to Split Pane",
        )
        if not self.is_alive():
            fail("iTerm2 exited after move-pane")
        print("OK: Move Session to Split Pane sent move-pane", flush=True)

        splits_before = self.log_text().count("split-window")
        self.keystroke('"d"', "command down")
        self.wait_log(
            lambda text: text.count("split-window") > splits_before,
            20,
            "split-window after move-pane",
        )
        try:
            self.click_menu("Session", "Move Session", "Move Session to Window")
        except subprocess.CalledProcessError as exc:
            fail(f"Move Session to Window menu failed: {exc.output or exc}")
        self.wait_log(
            lambda text: any(cmd.startswith("break-pane") for cmd in control_commands(text)),
            20,
            "break-pane after Move Session to Window",
        )
        if not self.is_alive():
            fail("iTerm2 exited after break-pane")
        print("OK: Move Session to Window sent break-pane", flush=True)

    def after_marker(self, marker: str) -> None:
        self.dump_visible("02-after-marker", require=marker)

    def after_layout_suite(self) -> None:
        self.select_first_tab()
        self.key_code(53)  # escape
        time.sleep(1.5)
        if not pids_named("htmd"):
            fail("htmd exited on gateway Esc; expected detach, not shutdown")
        if not ipc_path().exists():
            fail("IPC socket vanished on Esc detach; htmd should keep listening")
        if not self.is_alive():
            fail("iTerm2 exited on HTM detach")
        print("OK: Esc detached without killing htmd", flush=True)

        self.keystroke('"t"', "command down")
        time.sleep(1.5)
        self.keystroke(f'"{self.htm}"')
        self.key_code(36)
        self.started_at = time.time() - 1.0
        self.wait_init(timeout=20)
        time.sleep(1.0)
        print("OK: reattached", flush=True)
        reattach = self.log_text()
        if not any(
            "@iterm2_id" in cmd and cmd.startswith("show")
            for cmd in control_commands(reattach)
        ):
            fail("reattach did not show @iterm2_id")
        set_after = [
            cmd
            for cmd in commands_containing(reattach, "@iterm2_id")
            if cmd.startswith("set")
        ]
        if len(set_after) > len(self.iterm2_set_ids):
            fail(
                "reattach issued a new @iterm2_id set; session option did not persist "
                f"(before={len(self.iterm2_set_ids)} after={len(set_after)})"
            )
        print("OK: reattach reused persisted @iterm2_id", flush=True)

    def warn_leftovers(self) -> None:
        if self.proc and self.proc.poll() is None:
            fail("suite iTerm2 did not exit after stop()")
        still = [
            pid
            for pid in self._iterm_pids()
            if pid not in self.preexisting_iterm and SUITE in self._pid_command(pid)
        ]
        if still:
            print(f"WARN: leftover iTerm2 pids {still}", flush=True)


def main() -> int:
    return run_emulator_main(sys.modules[__name__], default_suite="layout")


if __name__ == "__main__":
    raise SystemExit(main())
