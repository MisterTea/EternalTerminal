#!/usr/bin/env python3
"""End-to-end tests for htm/htmd native Ghostty integration.

Launches a locally built Ghostty with an isolated ``--config-file`` so the
user's ``~/.config/ghostty`` and installed ``/Applications/Ghostty.app`` are
left alone. Prefers ``~/github/ghostty/macos/build/Debug/Ghostty.app`` (bundle
id ``com.mitchellh.ghostty.debug``) over the release app.

Protocol correctness is checked against htmd logs; native splits/tabs are
driven with the same Cmd+D / Cmd+T / Cmd+Shift+D keys as the iTerm2 suite.

Skip (exit 77) when Ghostty, htm/htmd, or Accessibility is unavailable.
Not registered with default CTest; run this file directly (see AGENTS.md).
Expects Ghostty's tmux -CC viewer (DCS 1000p from ``htm``).

Environment:
  GHOSTTY_BIN  Path to a ``ghostty`` binary
  GHOSTTY_APP  Path to Ghostty.app (debug build recommended)
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
    _is_gateway_title,
    assert_control_mode_attached,
    command_count,
    fail,
    kill_htm_daemons,
    normalize_gateway_text,
    run_emulator_main,
    run_osascript,
    skip,
    wait_until,
)

HERE = Path(__file__).resolve().parent
CONFIG_FILE = HERE / "ghostty_htm_e2e.config"
STDERR_LOG = Path("/tmp") / "ghostty-htm-e2e.stderr"
GHOSTTY_SRC = Path.home() / "github" / "ghostty"


def ghostty_stderr_excerpt() -> str:
    try:
        text = STDERR_LOG.read_text(errors="replace")
    except OSError:
        return ""
    return text[-1500:]


def fail_ghostty_exit(when: str) -> None:
    fail(
        f"Ghostty exited {when}. tmux -CC attach may have failed. "
        f"stderr:\n{ghostty_stderr_excerpt()}"
    )


def is_ghostty_app(app: Path) -> bool:
    return (app / "Contents" / "MacOS" / "ghostty").is_file()


def is_ghostty_binary(path: Path) -> bool:
    return path.is_file() and os.access(path, os.X_OK) and "ghostty" in path.name.lower()


def candidate_ghostty() -> list[Path]:
    paths: list[Path] = []
    for env in ("GHOSTTY_BIN", "GHOSTTY_APP"):
        val = os.environ.get(env)
        if val:
            paths.append(Path(val))
    paths.extend(
        [
            GHOSTTY_SRC / "macos" / "build" / "Debug" / "Ghostty.app",
            GHOSTTY_SRC / "zig-out" / "Ghostty.app",
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


def find_ghostty_app() -> Path:
    for path in candidate_ghostty():
        if path.is_dir() and is_ghostty_app(path):
            return path
        if is_ghostty_binary(path):
            return path
    skip(
        "no local Ghostty found; build ~/github/ghostty "
        "(macos/build.nu Debug) or set GHOSTTY_BIN / GHOSTTY_APP. "
        "Will not launch /Applications/Ghostty.app."
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


NAME = "Ghostty"


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--ghostty")
    parser.add_argument("--ghostty-app")


def apply_args(args: argparse.Namespace) -> None:
    if args.ghostty:
        os.environ["GHOSTTY_BIN"] = args.ghostty
    if args.ghostty_app:
        os.environ["GHOSTTY_APP"] = args.ghostty_app
    if not CONFIG_FILE.is_file():
        fail(f"missing {CONFIG_FILE}")


def open_session(htm: Path, htmd: Path, args: argparse.Namespace) -> "GhosttyHtmSession":
    return GhosttyHtmSession(find_ghostty_app(), htm, htmd)


class GhosttyHtmSession(GuiTerminalSession):
    name = "Ghostty"
    supports_native_resize = True

    def __init__(self, app: Path, htm: Path, htmd: Path):
        super().__init__(htm, htmd)
        self.app = app
        self.proc: Optional[subprocess.Popen] = None
        self.gui_pid: Optional[int] = None
        self.preexisting_gui = set(self._ghostty_pids())
        self.stderr_file = None

    def _ghostty_pids(self) -> list[int]:
        pids = []
        try:
            out = subprocess.check_output(["pgrep", "-x", "ghostty"], text=True)
        except subprocess.CalledProcessError:
            return []
        for line in out.split():
            try:
                pids.append(int(line))
            except ValueError:
                continue
        return pids

    def ghostty_bin(self) -> Path:
        app = self.app
        if app.is_dir() and (app.suffix == ".app" or str(app).endswith(".app")):
            return app / "Contents" / "MacOS" / "ghostty"
        return app

    def resolve_gui_pid(self) -> int:
        if self.gui_pid and pid_comm(self.gui_pid):
            return self.gui_pid
        if self.proc and self.proc.poll() is None:
            comm = pid_comm(self.proc.pid)
            if comm.endswith("ghostty"):
                self.gui_pid = self.proc.pid
                return self.gui_pid
            for pid in descendant_pids(self.proc.pid):
                if pid_comm(pid).endswith("ghostty"):
                    self.gui_pid = pid
                    return pid
            self.gui_pid = self.proc.pid
            return self.gui_pid
        for pid in self._ghostty_pids():
            if pid not in self.preexisting_gui:
                self.gui_pid = pid
                return pid
        fail("Ghostty process is not running")

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
        """Best-effort native tab count for the front Ghostty window."""
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

    def reattach_client(self) -> None:
        # After Esc, the htm client can remain alive while htmd is already
        # idle (stdout backpressure / DCS teardown). Clear descendant htm
        # clients so the `htm; exec $SHELL` wrapper can reach the shell
        # before we paste attach. Leave htmd alone so the session survives.
        self.focus_gateway()
        if self.proc is not None:
            for pid in descendant_pids(self.proc.pid):
                try:
                    if "htm" in (pid_comm(pid) or "") and "htmd" not in (
                        pid_comm(pid) or ""
                    ):
                        os.kill(pid, signal.SIGTERM)
                except OSError:
                    pass
        deadline = time.time() + 12

        def _shell_ready() -> bool:
            windows = self.ax_windows()
            names = [(w.get("name") or "") for w in windows]
            if any(
                n and not _is_gateway_title(n) and n != "ghostty" for n in names
            ):
                return True
            try:
                text = normalize_gateway_text(self.gateway_text())
            except Exception:
                return False
            # Avoid false positives from dumped `%begin` / `%end` lines.
            return "➜" in text or ("Command Menu" not in text and "$ " in text)

        while time.time() < deadline and not _shell_ready():
            time.sleep(0.3)
        time.sleep(0.4)
        super().reattach_client()

    def multiplexer_command(self) -> str:
        # Keep a shell after tmux/htm exits so Esc detach leaves the gateway
        # usable for reattach (same pattern as WezTerm's e2e driver).
        command = super().multiplexer_command()
        shell = f'{command}; exec "${{SHELL:-/bin/zsh}}" -l'
        return f"/bin/sh -c {shlex.quote(shell)}"

    def start(self, command: str = "") -> None:
        if not CONFIG_FILE.is_file():
            fail(f"missing Ghostty e2e config {CONFIG_FILE}")
        kill_htm_daemons()
        self.started_at = time.time() - 1.0
        env = os.environ.copy()
        ghostty_dir = str(self.ghostty_bin().parent)
        env["PATH"] = f"{self.htm.parent}:{ghostty_dir}:{env.get('PATH', '')}"
        STDERR_LOG.parent.mkdir(parents=True, exist_ok=True)
        self.stderr_file = open(STDERR_LOG, "w")
        binary = self.ghostty_bin()
        self.proc = subprocess.Popen(
            [
                str(binary),
                "--config-default-files=false",
                f"--config-file={CONFIG_FILE}",
                "-e",
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
            description="Ghostty process start",
        )

        def window_ready() -> bool:
            if self.proc is not None and self.proc.poll() is not None:
                if self.stderr_file:
                    try:
                        self.stderr_file.flush()
                    except OSError:
                        pass
                fail_ghostty_exit("before opening a window")
            return self.window_count() > 0

        wait_until(window_ready, 25, description="Ghostty window")
        self.remember_gateway_windows()
        self.focus()

    def previous_pane(self) -> None:
        self.keystroke('"["', "command down")
        time.sleep(0.5)

    def next_pane(self) -> None:
        self.keystroke('"]"', "command down")
        time.sleep(0.5)

    def after_attach(self) -> None:
        if self.mux == "tmux":
            wait_until(
                lambda: self.tmux_has_session()
                and self.proc is not None
                and self.proc.poll() is None,
                15,
                description="Ghostty tmux -CC attach",
            )
        else:
            self.wait_log(
                lambda text: "list-panes" in text,
                15,
                "Ghostty list-panes after attach",
            )
        if self.proc is not None and self.proc.poll() is not None:
            fail_ghostty_exit("after tmux -CC attach")
        self.gui_pid = None
        try:
            self.focus()
        except subprocess.CalledProcessError:
            pass
        # Requires iTerm2-compatible control plate + native mux surfaces.
        assert_control_mode_attached(self)
        # assert_control_mode reads the menu via gateway_text(), which
        # leaves key focus on the control plate (Cmd+1). Corners/layout
        # typing should target a follower pane; drain captures first so
        # send-keys are not stuck behind the attach snapshot.
        time.sleep(1.0)
        self.focus_mux_surface()

    def focus_gateway(self) -> None:
        """Raise the control-plane OS window (no mux tabs share it)."""
        super().focus_gateway()
        time.sleep(0.2)

    def htm_select_window(self, wid: int) -> None:
        """Send ``select-window`` through Ghostty's control-plate prompt.

        HTM is a single control-mode client (the emulator). A side-channel
        ``htm -C`` would steal the socket; the gateway ``C`` prompt talks on
        the same connection Ghostty already holds so ``%session-window-changed``
        reaches libghostty for Cmd+T affinity.
        """
        self.focus_gateway()
        time.sleep(0.2)
        # Bare ``c`` opens the tmux/htm command prompt on the plate.
        self.keystroke('"c"')
        time.sleep(0.2)
        self.osascript_pid(
            "set frontmost to true\n"
            f'    keystroke "select-window -t @{int(wid)}"\n'
            "    key code 36"
        )
        time.sleep(0.45)

    def gateway_text(self) -> str:
        """Read gateway visible text via select-all + clipboard."""
        self.focus_gateway()
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

    def focus_mux_surface(self) -> None:
        """Raise a native mux window, not the control plate."""
        self.focus_native_window()
        time.sleep(0.35)

    def resize_front_native_window(self, width: int, height: int) -> None:
        """Resize the mux OS window (not the control-plane gateway)."""
        self.focus_native_window()
        launched = self.launched_windows()
        if launched:
            # Prefer an explicit mux window index over "front window", which
            # can still be the gateway after a focus race.
            idx = int(launched[0]["index"])
            self.osascript_pid(
                "set frontmost to true\n"
                f"    set size of window {idx} to {{{width}, {height}}}"
            )
        else:
            self.osascript_pid(
                "set frontmost to true\n"
                f"    set size of front window to {{{width}, {height}}}"
            )
        time.sleep(0.3)

    def focus_native_window(self) -> None:
        """Raise a mux pane OS window (separate from the gateway)."""
        super().focus_native_window()

    def after_first_split(self) -> None:
        # Let capture-pane / list-panes drain so send-keys are not queued
        # behind viewer snapshots, then leave the control plate so pane
        # navigation (Cmd+] / previous_pane) targets a follower surface.
        time.sleep(1.0)
        self.focus_mux_surface()

    def keystroke(self, keys: str, using: str = "") -> None:
        using_clause = f" using {using}" if using else ""
        self.osascript_pid(
            f"set frontmost to true\n    keystroke {keys}{using_clause}"
        )
        time.sleep(0.12 if len(keys) < 20 else 0.25)

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
        # Never kill the user's installed Ghostty. Only this invocation
        # and ghostty children that were not already running.
        targets = []
        if self.proc:
            targets.append(self.proc.pid)
            targets.extend(descendant_pids(self.proc.pid))
        if self.gui_pid:
            targets.append(self.gui_pid)
        for pid in self._ghostty_pids():
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
            fail("Ghostty did not send list-windows after control-mode attach")
        print("OK: Ghostty enumerated tmux windows", flush=True)

    def warn_leftovers(self) -> None:
        still = [
            pid
            for pid in self._ghostty_pids()
            if pid not in self.preexisting_gui
        ]
        if still:
            print(f"WARN: leftover ghostty pids {still}", flush=True)


def main() -> int:
    return run_emulator_main(sys.modules[__name__], default_suite="layout")


if __name__ == "__main__":
    raise SystemExit(main())
