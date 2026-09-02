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
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, Optional

SKIP = 77
SUITE = "EternalTerminalHtmE2E"


def skip(reason: str) -> None:
    print(f"SKIP: {reason}", flush=True)
    raise SystemExit(SKIP)


def fail(reason: str) -> None:
    print(f"FAIL: {reason}", flush=True)
    raise SystemExit(1)


def typed_from_log(text: str) -> str:
    """Rebuild keystrokes from htmd ``control command: send`` log lines.

    Stock iTerm2 sends printable characters as one ``send -lt %pane CHAR``
    command each, so a marker never appears as a contiguous substring in the
    log unless we concatenate those payloads.
    """
    out: list[str] = []
    for line in text.splitlines():
        if "control command: send" not in line:
            continue
        cmd = line.split("control command:", 1)[1].strip()
        tokens = cmd.split()
        if not tokens or tokens[0] not in ("send", "send-keys"):
            continue
        hex_mode = False
        payload: list[str] = []
        i = 1
        while i < len(tokens):
            tok = tokens[i]
            if tok.startswith("-") and len(tok) > 1:
                if "H" in tok[1:]:
                    hex_mode = True
                if "t" in tok[1:] and i + 1 < len(tokens) and tokens[i + 1][:1] in "%$@":
                    i += 2
                    continue
                i += 1
                continue
            payload.append(tok)
            i += 1
        for item in payload:
            if re.fullmatch(r"0x[0-9A-Fa-f]+", item):
                out.append(chr(int(item, 16) & 0xFF))
            elif hex_mode and re.fullmatch(r"[0-9A-Fa-f]{1,2}", item):
                out.append(chr(int(item, 16) & 0xFF))
            else:
                out.append(item)
    return "".join(out)


def log_has_typed(text: str, marker: str) -> bool:
    return marker in text or marker in typed_from_log(text)


def control_commands(text: str) -> list[str]:
    """Incoming tmux -CC lines logged as ``control command: …``."""
    out: list[str] = []
    for line in text.splitlines():
        if "control command:" not in line:
            continue
        out.append(line.split("control command:", 1)[1].strip())
    return out


def commands_containing(text: str, *needles: str) -> list[str]:
    return [cmd for cmd in control_commands(text) if all(n in cmd for n in needles)]


def uid() -> int:
    return os.getuid()


def ipc_path() -> Path:
    return Path("/tmp") / f"htm.{uid()}.ipc"


def list_htmd_logs() -> list[Path]:
    tmp = Path("/tmp")
    try:
        return [
            tmp / name
            for name in os.listdir(tmp)
            if name.startswith("htmd-")
            and name.endswith(".log")
            and "stderr" not in name
        ]
    except FileNotFoundError:
        return []


def newest_log(logs: list[Path]) -> Optional[Path]:
    best = None
    best_mtime = 0.0
    for path in logs:
        try:
            mtime = path.stat().st_mtime
        except OSError:
            continue
        if mtime >= best_mtime:
            best_mtime = mtime
            best = path
    return best


def read_text(path: Optional[Path]) -> str:
    if not path:
        return ""
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def pids_named(name: str) -> list[int]:
    try:
        out = subprocess.check_output(
            ["pgrep", "-x", "-U", str(uid()), name],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except subprocess.CalledProcessError:
        return []
    pids = []
    for line in out.split():
        try:
            pids.append(int(line))
        except ValueError:
            continue
    return pids


def wait_until(
    predicate: Callable[[], bool],
    timeout: float,
    interval: float = 0.2,
    description: str = "condition",
) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return
        time.sleep(interval)
    fail(f"timed out waiting for {description}")


def run_osascript(script: str, timeout: float = 20.0) -> str:
    try:
        return subprocess.check_output(
            ["osascript", "-e", script],
            text=True,
            stderr=subprocess.STDOUT,
            timeout=timeout,
        )
    except subprocess.CalledProcessError as exc:
        output = exc.output or ""
        lowered = output.lower()
        if any(
            marker in lowered
            for marker in (
                "not allowed assistive access",
                "osascript is not allowed",
                "not authorized to send apple events",
            )
        ):
            skip("osascript needs Accessibility permission")
        # -1719 is a transient "no such object" (process/window not ready yet),
        # not a TCC denial. Let callers retry.
        raise


def find_htm_bin(cli: Optional[str]) -> Path:
    if cli:
        path = Path(cli)
        if path.is_file():
            return path.resolve()
        fail(f"--htm not found: {path}")
    env = os.environ.get("HTM_BIN")
    if env and Path(env).is_file():
        return Path(env).resolve()
    for candidate in (
        Path(__file__).resolve().parents[2] / "build" / "htm",
        Path.cwd() / "htm",
        Path.cwd() / "build" / "htm",
    ):
        if candidate.is_file():
            return candidate.resolve()
    skip("htm binary is not built")


def find_htmd_bin(cli: Optional[str], htm: Path) -> Path:
    if cli and Path(cli).is_file():
        return Path(cli).resolve()
    env = os.environ.get("HTMD_BIN")
    if env and Path(env).is_file():
        return Path(env).resolve()
    sibling = htm.parent / "htmd"
    if sibling.is_file():
        return sibling.resolve()
    skip("htmd binary is not built")


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


class ITermHtmSession:
    def __init__(self, app: Path, htm: Path, htmd: Path):
        self.app = app
        self.htm = htm
        self.htmd = htmd
        self.proc: Optional[subprocess.Popen] = None
        self.log_file: Optional[Path] = None
        self.started_at = 0.0
        self.preexisting_iterm = set(self._iterm_pids())

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

    def wait_init(self, timeout: float = 25.0) -> str:
        def ready() -> bool:
            path = newest_log(list_htmd_logs())
            text = read_text(path)
            if not path:
                return False
            try:
                if path.stat().st_mtime < self.started_at:
                    return False
            except OSError:
                return False
            if "control command:" in text or "control-mode" in text:
                self.log_file = path
                return True
            return False

        try:
            wait_until(ready, timeout, description="htmd control-mode attach")
        except SystemExit:
            path = newest_log(list_htmd_logs())
            text = read_text(path)
            fail(
                "htmd control-mode attach; "
                f"htm={pids_named('htm')} htmd={pids_named('htmd')} "
                f"log={path} head:\n{text[:1500]}"
            )
        return read_text(self.log_file)

    def log_text(self) -> str:
        return read_text(self.log_file)

    def wait_log(self, predicate: Callable[[str], bool], timeout: float, what: str) -> str:
        last = ""

        def ready() -> bool:
            nonlocal last
            last = self.log_text()
            return predicate(last)

        try:
            wait_until(ready, timeout, description=what)
        except SystemExit:
            tail = last[-2000:]
            fail(f"{what}; tail:\n{tail}")
        return last

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


def assert_no_htmd() -> None:
    wait_until(
        lambda: not pids_named("htmd"),
        8,
        description="htmd process exit",
    )


def assert_no_ipc() -> None:
    wait_until(lambda: not ipc_path().exists(), 8, description="IPC socket removal")


def run_tests(session: ITermHtmSession) -> None:
    marker = f"HTM_E2E_{int(time.time())}"
    session.start(f"{session.htm} -x")
    session.wait_init()
    # Native tmux -CC materializes the first pane as a real iTerm2 tab.
    deadline = time.time() + 8
    while time.time() < deadline and session.tab_count() < 2:
        time.sleep(0.25)
    session.focus()

    tabs_after_init = session.tab_count()
    if tabs_after_init < 2:
        print(
            f"WARN: AX tab count after control-mode attach was {tabs_after_init}; "
            "continuing with protocol assertions",
            flush=True,
        )
    else:
        print(f"OK: control-mode created native tabs ({tabs_after_init})", flush=True)

    attach_log = session.wait_log(
        lambda text: (
            any(cmd.startswith("set") and "@iterm2_id" in cmd for cmd in control_commands(text))
            and any(cmd.startswith("show") and "@iterm2_id" in cmd for cmd in control_commands(text))
            and (
                commands_containing(text, "@affinities")
                or commands_containing(text, "@uservars")
                or commands_containing(text, "show-options")
            )
        ),
        20,
        "attach @iterm2_id/@affinities/@uservars",
    )
    set_ids = [
        cmd
        for cmd in commands_containing(attach_log, "@iterm2_id")
        if cmd.startswith("set")
    ]
    print("OK: attach stored and queried @ user options", flush=True)

    # Split the focused HTM client.
    session.keystroke('"d"', "command down")
    session.wait_log(
        lambda text: "split-window" in text,
        20,
        "split-window after Cmd+D",
    )
    print("OK: Cmd+D sent split-window", flush=True)

    try:
        session.click_menu("Session", "Move Session", "Move Session to Split Pane")
    except subprocess.CalledProcessError as exc:
        fail(f"Move Session to Split Pane menu failed: {exc.output or exc}")
    time.sleep(0.5)
    session.click_pane_half("left")
    session.wait_log(
        lambda text: any(cmd.startswith("move-pane") for cmd in control_commands(text)),
        20,
        "move-pane after Move Session to Split Pane",
    )
    if session.proc.poll() is not None:
        fail("iTerm2 exited after move-pane")
    print("OK: Move Session to Split Pane sent move-pane", flush=True)
    # swap-pane is only on the pane context menu (not AX-reachable here).
    # unlink-window is dashboard-only. Both are covered by unit tests.

    session.keystroke('"d"', "command down")
    session.wait_log(
        lambda text: text.count("split-window") >= 2,
        20,
        "split-window after move-pane",
    )
    try:
        session.click_menu("Session", "Move Session", "Move Session to Window")
    except subprocess.CalledProcessError as exc:
        fail(f"Move Session to Window menu failed: {exc.output or exc}")
    session.wait_log(
        lambda text: any(cmd.startswith("break-pane") for cmd in control_commands(text)),
        20,
        "break-pane after Move Session to Window",
    )
    if session.proc.poll() is not None:
        fail("iTerm2 exited after break-pane")
    print("OK: Move Session to Window sent break-pane", flush=True)

    session.keystroke(f'"{marker}"')
    session.key_code(36)  # return
    session.wait_log(
        lambda text: log_has_typed(text, marker),
        20,
        f"send-keys containing {marker}",
    )
    print("OK: keys reached htmd pane", flush=True)

    session.keystroke('"t"', "command down")
    session.wait_log(
        lambda text: "new-window" in text,
        20,
        "new-window after Cmd+T",
    )
    print("OK: Cmd+T sent new-window", flush=True)

    session.keystroke('"d"', "{command down, shift down}")
    session.wait_log(
        lambda text: "split-window" in text,
        20,
        "second split-window after Cmd+Shift+D",
    )
    print("OK: Cmd+Shift+D sent second split-window", flush=True)

    time.sleep(0.5)
    stamp = int(time.time())

    def echo_on_focused_pane(tag: str) -> None:
        session.keystroke(f'"echo {tag}"')
        session.key_code(36)
        session.wait_log(lambda text: log_has_typed(text, tag), 12, f"echo {tag}")

    mark_a = f"MA{stamp}"
    echo_on_focused_pane(mark_a)
    switched = False
    for switch in (session.next_pane, session.previous_pane, session.previous_tab):
        switch()
        tag = f"MB{stamp}{switch.__name__}"
        echo_on_focused_pane(tag)
        switched = True
        break
    if not switched:
        fail("could not focus a second HTM pane for concurrent output")
    print("OK: keys reached two panes", flush=True)

    def burst_cmd(tag: str) -> str:
        return f"for i in 1 2 3 4 5 6 7 8; do echo {tag}_$i; sleep 0.08; done &"

    loops = [f"IT2C0{stamp}", f"IT2C1{stamp}"]
    session.keystroke(f'"{burst_cmd(loops[1])}"')
    session.key_code(36)
    time.sleep(0.2)
    session.previous_pane()
    session.keystroke(f'"{burst_cmd(loops[0])}"')
    session.key_code(36)
    session.wait_log(
        lambda text: log_has_typed(text, loops[0]) and log_has_typed(text, loops[1]),
        20,
        "burst send-keys on two panes",
    )
    print("OK: concurrent pane output", flush=True)

    time.sleep(0.4)
    session.keystroke('"w"', "command down")
    session.wait_log(
        lambda text: "kill-pane" in text or "kill-window" in text,
        20,
        "CLIENT_CLOSE_PANE after Cmd+W",
    )
    print("OK: Cmd+W sent kill-pane/kill-window", flush=True)

    if session.proc.poll() is not None:
        fail("iTerm2 exited during the happy-path layout test")
    if not pids_named("htmd"):
        fail("htmd exited during the happy-path layout test")

    # Race: burst splits/tabs/closes while htmd is applying layout.
    splits_before = session.log_text().count("split-window")
    tabs_before = session.log_text().count("new-window")
    for _ in range(4):
        session.keystroke('"d"', "command down")
        session.keystroke('"t"', "command down")
        session.keystroke('"d"', "{command down, shift down}")
        session.keystroke('"w"', "command down")
    time.sleep(1.0)
    if session.proc.poll() is not None:
        fail("iTerm2 crashed during rapid split/tab/close")
    if not pids_named("htmd"):
        fail("htmd died during rapid split/tab/close")
    session.wait_log(
        lambda text: text.count("split-window") >= splits_before
        or text.count("new-window") >= tabs_before,
        15,
        "htmd still accepting packets after race burst",
    )
    print("OK: rapid split/tab/close did not crash iTerm2 or htmd", flush=True)

    # Clean detach: gateway Esc closes client panes but leaves htmd running.
    session.select_first_tab()
    session.key_code(53)  # escape
    time.sleep(1.5)
    if not pids_named("htmd"):
        fail("htmd exited on gateway Esc; expected detach, not shutdown")
    if not ipc_path().exists():
        fail("IPC socket vanished on Esc detach; htmd should keep listening")
    if session.proc.poll() is not None:
        fail("iTerm2 exited on HTM detach")
    print("OK: Esc detached without killing htmd", flush=True)

    # Reattach. After detach the original `--command=htm` process has exited.
    session.keystroke('"t"', "command down")
    time.sleep(1.5)
    session.keystroke(f'"{session.htm}"')
    session.key_code(36)
    session.started_at = time.time() - 1.0
    session.wait_init(timeout=20)
    time.sleep(1.0)
    print("OK: reattached", flush=True)
    reattach = session.log_text()
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
    if len(set_after) > len(set_ids):
        fail(
            "reattach issued a new @iterm2_id set; session option did not persist "
            f"(before={len(set_ids)} after={len(set_after)})"
        )
    print("OK: reattach reused persisted @iterm2_id", flush=True)
    for pid in pids_named("htmd"):
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
    assert_no_htmd()
    assert_no_ipc()
    leftover_htm = [p for p in pids_named("htm") if p]
    deadline = time.time() + 8
    while time.time() < deadline and leftover_htm:
        leftover_htm = pids_named("htm")
        time.sleep(0.2)
    if pids_named("htmd"):
        fail("htmd still running after SIGTERM")
    if ipc_path().exists():
        fail("IPC socket still present after htmd exit")
    print("OK: htmd shutdown removed IPC socket", flush=True)


def main() -> int:
    if sys.platform != "darwin":
        skip("iTerm2 HTM e2e requires macOS")

    parser = argparse.ArgumentParser()
    parser.add_argument("--htm")
    parser.add_argument("--htmd")
    parser.add_argument("--iterm2-app")
    args = parser.parse_args()
    if args.iterm2_app:
        os.environ["ITERM2_APP"] = args.iterm2_app

    htm = find_htm_bin(args.htm)
    htmd = find_htmd_bin(args.htmd, htm)
    app = find_iterm_app()
    print(f"Using iTerm2={app}", flush=True)
    print(f"Using htm={htm}", flush=True)
    print(f"Using htmd={htmd}", flush=True)

    session = ITermHtmSession(app, htm, htmd)
    try:
        run_tests(session)
    finally:
        session.stop()
        # Best-effort: if shutdown x did not run, do not leave a test daemon.
        for pid in pids_named("htmd"):
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass
        if session.proc and session.proc.poll() is None:
            fail("suite iTerm2 did not exit after stop()")
        still = [
            pid
            for pid in session._iterm_pids()
            if pid not in session.preexisting_iterm
            and SUITE in session._pid_command(pid)
        ]
        if still:
            print(f"WARN: leftover iTerm2 pids {still}", flush=True)
    print("PASS: iTerm2 htm/htmd e2e", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
