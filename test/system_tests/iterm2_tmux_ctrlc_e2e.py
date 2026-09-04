#!/usr/bin/env python3
"""Autonomous macOS visual tests: Ctrl+C through tmux and tmux -CC in iTerm2.

Launches a private iTerm2 prefs suite so the user's windows are left alone,
drives the GUI with System Events, captures the focused window with
``screencapture``, and reads the visible buffer via Cmd+A / Cmd+C / ``pbpaste``.
That is the same technique as the HTM iTerm2 e2e: no human judgment, no
headless PTY stand-in for the native tmux integration.

Skip (exit 77) off macOS, without a GUI session, without iTerm2, or without
tmux. Not registered with default CTest. Run:

  python3 test/system_tests/iterm2_tmux_ctrlc_e2e.py

Environment:
  ITERM2_APP   Path to iTerm2.app (default: /Applications/iTerm.app)
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, Optional

SKIP = 77
SUITE = "EternalTerminalTmuxCtrlCE2E"
CAPTURE_DIR = Path("/tmp/iterm2-tmux-ctrlc-e2e")
PROTOCOL_LEAK_RE = re.compile(
    r"%(?:output|extended-output|layout-change|session-changed|"
    r"window-add|begin|end)\b"
)
SPAM = "ET_CTRLC_SPAM"
ATTACHED = "ET_CTRLC_ATTACHED"
AFTER = "ET_CTRLC_AFTER"


def skip(reason: str) -> None:
    print(f"SKIP: {reason}", flush=True)
    raise SystemExit(SKIP)


def fail(reason: str) -> None:
    print(f"FAIL: {reason}", flush=True)
    raise SystemExit(1)


class WaitTimeout(Exception):
    pass


def wait_until(
    predicate: Callable[[], bool],
    timeout: float,
    interval: float = 0.25,
    description: str = "condition",
) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return
        time.sleep(interval)
    raise WaitTimeout(description)


def run_osascript(script: str, timeout: float = 20.0) -> str:
    try:
        return subprocess.check_output(
            ["osascript", "-e", script],
            text=True,
            stderr=subprocess.STDOUT,
            timeout=timeout,
        )
    except subprocess.CalledProcessError as exc:
        output = (exc.output or "").lower()
        if any(
            marker in output
            for marker in (
                "not allowed assistive access",
                "osascript is not allowed",
                "not authorized to send apple events",
            )
        ):
            skip("osascript needs Accessibility permission")
        raise


def applescript_quote(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def find_iterm_app() -> Path:
    env = os.environ.get("ITERM2_APP")
    candidates = []
    if env:
        candidates.append(Path(env))
    candidates.extend(
        [
            Path("/Applications/iTerm.app"),
            Path("/Applications/iTerm2.app"),
        ]
    )
    try:
        found = subprocess.check_output(
            ["mdfind", "kMDItemCFBundleIdentifier == 'com.googlecode.iterm2'"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
        for line in found.splitlines():
            if line.strip():
                candidates.append(Path(line.strip()))
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    seen = set()
    for path in candidates:
        if not path.is_dir():
            continue
        resolved = path.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        if (path / "Contents" / "MacOS" / "iTerm2").is_file():
            return path
    skip("no iTerm2.app found; install iTerm2 or set ITERM2_APP")


def configure_suite_defaults() -> None:
    bools = {
        "EnableAPIServer": True,
        "PromptOnQuit": False,
        "OnlyWhenMoreTabs": False,
        "OpenArrangementAtStartup": False,
        "OpenNoWindowsAtStartup": True,
        "SUEnableAutomaticChecks": False,
        "NoSyncNeverRemindPrefsChangesAgain": True,
        "HideTab": False,
        # Hide the tmux client gateway so keystrokes land in the native pane.
        "AutoHideTmuxClientSession": True,
    }
    for key, enabled in bools.items():
        subprocess.run(
            ["defaults", "write", SUITE, key, "-bool", "true" if enabled else "false"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


class ITermTmuxSession:
    def __init__(self, app: Path, tmux: Path, control_mode: bool):
        self.app = app
        self.tmux = tmux
        self.control_mode = control_mode
        self.socket = f"et_tmux_ctrlc_{os.getpid()}_{'cc' if control_mode else 'tty'}"
        self.proc: Optional[subprocess.Popen] = None
        self.preexisting_iterm = set(self._iterm_pids())
        self._capturing_text = False
        self.label = "tmux-cc" if control_mode else "tmux"

    def _iterm_pids(self) -> list[int]:
        try:
            out = subprocess.check_output(["pgrep", "-f", "iTerm2"], text=True)
        except subprocess.CalledProcessError:
            return []
        pids = []
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

    def osascript_pid(self, body: str) -> str:
        script = f"""
tell application "System Events"
  tell (first process whose unix id is {self.pid})
    {body}
  end tell
end tell
"""
        try:
            return run_osascript(script)
        except subprocess.CalledProcessError:
            time.sleep(0.3)
            return run_osascript(script)

    @property
    def pid(self) -> int:
        if not self.proc or self.proc.poll() is not None:
            fail("iTerm2 process is not running")
        return self.proc.pid

    def focus(self) -> None:
        self.osascript_pid("set frontmost to true")
        time.sleep(0.12)

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

    def type_line(self, text: str) -> None:
        self.keystroke(applescript_quote(text))
        self.key_code(36)

    def window_count(self) -> int:
        try:
            return int(self.osascript_pid("get count of windows").strip())
        except (ValueError, subprocess.CalledProcessError):
            return 0

    def window_frame(self) -> tuple[float, float, float, float]:
        out = self.osascript_pid(
            "set p to position of window 1\n"
            "set s to size of window 1\n"
            'return (item 1 of p as text) & "," & (item 2 of p as text) & "," & '
            '(item 1 of s as text) & "," & (item 2 of s as text)'
        ).strip()
        x, y, width, height = [float(part) for part in out.split(",")]
        return x, y, width, height

    def screenshot(self, name: str) -> Path:
        out_dir = CAPTURE_DIR / self.label
        out_dir.mkdir(parents=True, exist_ok=True)
        path = out_dir / f"{name}.png"
        self.focus()
        time.sleep(0.15)
        x, y, width, height = self.window_frame()
        region = f"{int(x)},{int(y)},{int(width)},{int(height)}"
        subprocess.check_call(
            ["screencapture", "-R", region, "-x", str(path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        print(f"screenshot {self.label}/{name} -> {path}", flush=True)
        return path

    def visible_contents(self) -> str:
        """Copy the focused session via macOS clipboard (suite iTerm is not AEOM)."""
        self._capturing_text = True
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
            self._capturing_text = False
            subprocess.run(
                ["pbcopy"],
                input=previous,
                text=True,
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

    def dump_visible(self, name: str, require: Optional[str] = None) -> str:
        img = self.screenshot(name)
        text = self.visible_contents()
        dump = CAPTURE_DIR / self.label / f"{name}.txt"
        dump.parent.mkdir(parents=True, exist_ok=True)
        dump.write_text(text)
        preview_lines = [line[:160] for line in text.splitlines() if line.strip()]
        preview = "\n".join(f"  | {line}" for line in preview_lines[-25:])[:2000]
        print(
            f"visible[{self.label}/{name}] screenshot={img} chars={len(text)}\n{preview}",
            flush=True,
        )
        if self.control_mode:
            leak = PROTOCOL_LEAK_RE.search(text)
            if leak:
                fail(
                    f"control-mode {leak.group(0)!r} leaked into the native "
                    f"iTerm2 pane at {name}:\n{text[:2000]}"
                )
        if require is not None and require not in text:
            fail(
                f"expected {require!r} in visible iTerm2 text at {name} "
                f"(screenshot={img}):\n{text[:2000]}"
            )
        return text

    def wait_visible(self, needle: str, timeout: float, what: str) -> str:
        last = ""

        def ready() -> bool:
            nonlocal last
            last = self.visible_contents()
            return needle in last

        try:
            wait_until(ready, timeout, description=what)
        except WaitTimeout:
            self.screenshot(f"timeout-{what.replace(' ', '-')}")
            fail(f"{what}; last visible text:\n{last[:2000]}")
        return last

    def start(self) -> None:
        configure_suite_defaults()
        flags = "-CC " if self.control_mode else ""
        inner = (
            "export TERM=xterm-256color; "
            f"exec {shlex.quote(str(self.tmux))} -L {shlex.quote(self.socket)} "
            f"-f /dev/null {flags}new-session -x 80 -y 24"
        )
        command = f"/bin/sh -lc {shlex.quote(inner)}"
        env = os.environ.copy()
        env["PATH"] = f"{self.tmux.parent}:{env.get('PATH', '')}"
        env["IT2_SUITE"] = SUITE
        binary = self.app / "Contents" / "MacOS" / "iTerm2"
        self.proc = subprocess.Popen(
            [str(binary), "-suite", SUITE, f"--command={command}"],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        try:
            wait_until(
                lambda: self.proc is not None and self.proc.poll() is None,
                5,
                description="iTerm2 process start",
            )
            wait_until(
                lambda: self.window_count() > 0, 25, description="iTerm2 window"
            )
        except WaitTimeout as exc:
            fail(f"timed out waiting for {exc}")
        self.focus()
        if self.control_mode:
            # Native pane windows replace / join the gateway after DCS attach.
            deadline = time.time() + 8
            while time.time() < deadline and self.window_count() < 1:
                time.sleep(0.2)
            time.sleep(0.8)

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
        app_marker = str(self.app)
        for pid in self._iterm_pids():
            if pid in self.preexisting_iterm:
                continue
            if self.proc and pid == self.proc.pid:
                continue
            cmdline = self._pid_command(pid)
            if SUITE in cmdline or app_marker in cmdline:
                try:
                    os.kill(pid, signal.SIGTERM)
                except OSError:
                    pass
        subprocess.run(
            [str(self.tmux), "-L", self.socket, "kill-server"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        tmpdir = os.environ.get("TMUX_TMPDIR", "/tmp")
        socket_path = Path(tmpdir) / f"tmux-{os.getuid()}" / self.socket
        try:
            socket_path.unlink()
        except OSError:
            pass


def run_ctrl_c_scenario(session: ITermTmuxSession) -> None:
    mode = "tmux -CC" if session.control_mode else "tmux"
    print(f"=== {mode}: attach, flood, Ctrl+C, confirm prompt ===", flush=True)
    session.start()
    # Device-attribute replies can land in the pane as typed junk; cancel
    # them so the first echo is a clean command.
    session.keystroke('"c"', "control down")
    time.sleep(0.3)
    session.dump_visible("after-launch")
    session.type_line(f"echo {ATTACHED}")
    session.wait_visible(ATTACHED, 15, f"{ATTACHED} after attach")
    session.dump_visible("after-attach", require=ATTACHED)

    session.type_line(f"yes {SPAM}")
    session.wait_visible(SPAM, 10, f"{SPAM} flood on screen")
    session.screenshot("during-spam")

    interrupt_at = time.time()
    session.keystroke('"c"', "control down")
    time.sleep(0.35)
    session.type_line(f"echo {AFTER}")
    session.wait_visible(AFTER, 5, f"{AFTER} after Ctrl+C")
    latency = time.time() - interrupt_at
    session.dump_visible("after-ctrl-c", require=AFTER)
    if latency >= 5:
        fail(f"{mode}: prompt marker took {latency:.2f}s after Ctrl+C")
    print(f"OK: {mode} recovered in {latency:.2f}s", flush=True)


def require_macos_gui() -> None:
    if sys.platform != "darwin":
        skip("tmux/tmux -CC visual e2e is macOS-only")
    try:
        run_osascript(
            'tell application "System Events" to get name of first process'
        )
    except SystemExit:
        raise
    except Exception as exc:  # noqa: BLE001
        skip(f"no GUI session available ({exc})")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Autonomous iTerm2 visual tests for tmux and tmux -CC Ctrl+C"
    )
    parser.add_argument("--iterm2-app")
    parser.add_argument(
        "--mode",
        choices=("all", "tmux", "tmux-cc"),
        default="all",
        help="Which visual scenario to run (default: both)",
    )
    args = parser.parse_args()
    if args.iterm2_app:
        os.environ["ITERM2_APP"] = args.iterm2_app

    require_macos_gui()
    tmux = shutil.which("tmux")
    if not tmux:
        skip("tmux binary not found in PATH")
    app = find_iterm_app()
    print(f"Using iTerm2={app} tmux={tmux}", flush=True)
    CAPTURE_DIR.mkdir(parents=True, exist_ok=True)

    modes = []
    if args.mode in ("all", "tmux"):
        modes.append(False)
    if args.mode in ("all", "tmux-cc"):
        modes.append(True)

    for control_mode in modes:
        session = ITermTmuxSession(app, Path(tmux), control_mode)
        try:
            run_ctrl_c_scenario(session)
        finally:
            session.stop()

    print("PASS: iTerm2 tmux/tmux -CC Ctrl+C visual e2e", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
