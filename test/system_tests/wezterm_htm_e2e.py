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
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

from htm_gui_e2e import (  # noqa: E402
    GuiTerminalSession,
    command_count,
    fail,
    kill_htm_daemons,
    run_emulator_main,
    run_osascript,
    skip,
    wait_until,
)

HERE = Path(__file__).resolve().parent
CONFIG_LUA = HERE / "wezterm_htm_e2e.lua"
STDERR_LOG = Path("/tmp") / "wezterm-htm-e2e.stderr"


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
    paths.extend(
        [
            home / "github" / "wezterm" / "target" / "release" / "wezterm",
            home / "github" / "wezterm" / "target" / "debug" / "wezterm",
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


NAME = "WezTerm"


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


def open_session(htm: Path, htmd: Path, args: argparse.Namespace) -> "WezTermHtmSession":
    return WezTermHtmSession(find_wezterm_app(), htm, htmd)


class WezTermHtmSession(GuiTerminalSession):
    name = "WezTerm"

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

    def window_count(self) -> int:
        try:
            out = self.osascript_pid("get count of windows").strip()
            return int(out)
        except (ValueError, subprocess.CalledProcessError):
            return 0

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

    def start(self, command: str = "") -> None:
        if not CONFIG_LUA.is_file():
            fail(f"missing WezTerm e2e config {CONFIG_LUA}")
        kill_htm_daemons()
        self.started_at = time.time() - 1.0
        env = os.environ.copy()
        wezterm_dir = str(self.wezterm_bin().parent)
        env["PATH"] = f"{self.htm.parent}:{wezterm_dir}:{env.get('PATH', '')}"
        env["WEZTERM_LOG"] = "info"
        STDERR_LOG.parent.mkdir(parents=True, exist_ok=True)
        self.stderr_file = open(STDERR_LOG, "w")
        binary = self.wezterm_bin()
        self.proc = subprocess.Popen(
            [
                str(binary),
                "--config-file",
                str(CONFIG_LUA),
                "start",
                "--always-new-process",
                "--",
                str(self.htm),
                "-x",
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

    def after_attach(self) -> None:
        self.wait_log(
            lambda text: "list-panes" in text or "list-windows" in text,
            15,
            "WezTerm list-panes/list-windows",
        )
        time.sleep(1.0)
        if self.proc is not None and self.proc.poll() is not None:
            fail_wezterm_crash("after tmux -CC attach")
        self.gui_pid = None
        try:
            windows = self.window_count()
            if windows >= 2:
                self.osascript_pid(
                    f"set frontmost to true\n    perform action \"AXRaise\" of window {windows}"
                )
                time.sleep(0.3)
        except subprocess.CalledProcessError:
            pass
        try:
            tabs = self.tab_count()
            if tabs >= 2:
                self.osascript_pid(
                    "set frontmost to true\n"
                    f"    click radio button {tabs} of tab group 1 of window 1"
                )
                time.sleep(0.3)
        except subprocess.CalledProcessError:
            pass
        try:
            self.focus()
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

    def after_layout_suite(self) -> None:
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


def main() -> int:
    return run_emulator_main(sys.modules[__name__], default_suite="layout")


if __name__ == "__main__":
    raise SystemExit(main())
