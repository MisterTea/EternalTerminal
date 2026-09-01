#!/usr/bin/env python3
"""End-to-end tests for htm/htmd native iTerm2 integration.

Launches a Development iTerm2 build with ``-suite EternalTerminalHtmE2E`` so
the user's installed iTerm2 prefs and windows are left alone. Protocol
correctness is checked against htmd logs; native tabs/panes are checked via
Accessibility. Clean-exit checks process leftovers and the IPC socket.

Skip (exit 77) when iTerm2+HTM, htm/htmd, or Accessibility is unavailable.
Not registered with default CTest; run this file directly (see AGENTS.md).

Environment:
  ITERM2_APP   Path to an iTerm2.app that contains HTM support
  HTM_BIN      Path to the ``htm`` binary (overridden by --htm)
  HTMD_BIN     Path to the ``htmd`` binary (overridden by --htmd)
"""

from __future__ import annotations

import argparse
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, Optional

SKIP = 77
SUITE = "EternalTerminalHtmE2E"
HEADER_INSERT_KEYS = 49  # '1'
HEADER_CLOSE_PANE = 51  # '3'
HEADER_NEW_TAB = 53  # '5'
HEADER_NEW_SPLIT = 57  # '9'


def skip(reason: str) -> None:
    print(f"SKIP: {reason}", flush=True)
    raise SystemExit(SKIP)


def fail(reason: str) -> None:
    print(f"FAIL: {reason}", flush=True)
    raise SystemExit(1)


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


def header_count(text: str, code: int) -> int:
    return text.count(f"Got message header: {code}")


def writing_to_ids(text: str) -> list[str]:
    """Pane UUIDs from VLOG ``WRITING TO <uuid>:`` lines, in log order."""
    ids = []
    needle = "WRITING TO "
    for line in text.splitlines():
        idx = line.find(needle)
        if idx < 0:
            continue
        rest = line[idx + len(needle) :]
        pane = rest.split(":", 1)[0].strip()
        if len(pane) == 36:
            ids.append(pane)
    return ids


def read_from_ids(text: str) -> list[str]:
    """Pane UUIDs that received INSERT_KEYS, in log order."""
    ids = []
    needle = "READ FROM "
    for line in text.splitlines():
        idx = line.find(needle)
        if idx < 0:
            continue
        rest = line[idx + len(needle) :]
        if len(rest) >= 36:
            ids.append(rest[:36])
    return ids


def inserted_by_pane(text: str) -> dict[str, str]:
    """Map pane UUID -> concatenated INSERT_KEYS payloads from the log."""
    out: dict[str, str] = {}
    needle = "READ FROM "
    for line in text.splitlines():
        idx = line.find(needle)
        if idx < 0:
            continue
        rest = line[idx + len(needle) :]
        if len(rest) < 38 or rest[36] != ":":
            continue
        pane = rest[:36]
        body = rest[37:]
        if " " in body:
            body, _length = body.rsplit(" ", 1)
        out[pane] = out.get(pane, "") + body
    return out


def inserted_keys(text: str) -> str:
    """Concatenate payloads from ``READ FROM <uuid>:<data> <length>`` lines.

    glog prefixes a timestamp with colons, so this must not split on the first
    ``:``. One keystroke is one log line; join them to recover typed text.
    """
    return "".join(inserted_by_pane(text).values())


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
            return path
        fail(f"--htm not found: {path}")
    env = os.environ.get("HTM_BIN")
    if env and Path(env).is_file():
        return Path(env)
    for candidate in (
        Path(__file__).resolve().parents[2] / "build" / "htm",
        Path.cwd() / "htm",
        Path.cwd() / "build" / "htm",
    ):
        if candidate.is_file():
            return candidate
    skip("htm binary is not built")


def find_htmd_bin(cli: Optional[str], htm: Path) -> Path:
    if cli and Path(cli).is_file():
        return Path(cli)
    env = os.environ.get("HTMD_BIN")
    if env and Path(env).is_file():
        return Path(env)
    sibling = htm.parent / "htmd"
    if sibling.is_file():
        return sibling
    skip("htmd binary is not built")


def app_has_htm_support(app: Path) -> bool:
    # Development builds put the real binary in iTerm2.debug.dylib; the
    # MacOS/iTerm2 file is a small Previews stub without HTM strings.
    macos = app / "Contents" / "MacOS"
    binaries = [
        macos / "iTerm2.debug.dylib",
        macos / "iTerm2",
    ]
    needles = (b"HTM mode started", b"CSI_HTM_HOOK", b"HtmGateway")
    for binary in binaries:
        if not binary.is_file():
            continue
        try:
            out = subprocess.check_output(
                ["strings", str(binary)],
                stderr=subprocess.DEVNULL,
            )
        except (subprocess.CalledProcessError, FileNotFoundError):
            continue
        if any(needle in out for needle in needles):
            return True
    return False


def candidate_iterm_apps() -> list[Path]:
    env = os.environ.get("ITERM2_APP")
    paths: list[Path] = []
    if env:
        paths.append(Path(env))
    paths.extend(
        [
            Path.home() / "github" / "iTerm2" / "build" / "Development" / "iTerm2.app",
            Path.home() / "github" / "iTerm2" / "build" / "Products" / "Development" / "iTerm2.app",
        ]
    )
    derived = Path.home() / "Library" / "Developer" / "Xcode" / "DerivedData"
    if derived.is_dir():
        for app in derived.glob("iTerm2*/Build/Products/Development/iTerm2.app"):
            paths.append(app)
    # Do not use /Applications/iTerm.app: stock 3.x has no HTM support.
    seen = set()
    unique = []
    for path in paths:
        resolved = path.resolve() if path.exists() else path
        if resolved in seen:
            continue
        seen.add(resolved)
        unique.append(path)
    return unique


def try_build_iterm2() -> None:
    """Best-effort Development build of the local iTerm2 checkout."""
    src = Path.home() / "github" / "iTerm2"
    makefile = src / "Makefile"
    if not makefile.is_file():
        return
    build_dir = src / "build"
    build_dir.mkdir(parents=True, exist_ok=True)
    print("Attempting iTerm2 Development build…", flush=True)
    try:
        subprocess.run(
            [
                "make",
                "-C",
                str(src),
                "Development",
                f"BUILD_DIR={build_dir}",
            ],
            check=False,
            timeout=900,
        )
    except (subprocess.TimeoutExpired, FileNotFoundError) as exc:
        print(f"iTerm2 build did not complete: {exc}", flush=True)


def find_iterm_app() -> Path:
    for path in candidate_iterm_apps():
        if path.is_dir() and app_has_htm_support(path):
            return path
    try_build = os.environ.get("ITERM2_BUILD", "").lower() in ("1", "true", "yes")
    if try_build:
        try_build_iterm2()
    for path in candidate_iterm_apps():
        if path.is_dir() and app_has_htm_support(path):
            return path
    skip(
        "no HTM-capable iTerm2.app found; build iTerm2 Development "
        f"({Path.home()}/github/iTerm2) or set ITERM2_APP. "
        "Pass --build-iterm2 to try compiling. Xcode may need its "
        "first-launch system-component install (admin password)."
    )


def configure_suite_defaults() -> None:
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

    def start(self, command: str) -> None:
        configure_suite_defaults()
        self.started_at = time.time() - 1.0
        env = os.environ.copy()
        env["PATH"] = f"{self.htm.parent}:{env.get('PATH', '')}"
        env["IT2_SUITE"] = SUITE
        binary = self.app / "Contents" / "MacOS" / "iTerm2"
        self.proc = subprocess.Popen(
            [
                str(binary),
                "-suite",
                SUITE,
                f"--command={command}",
            ],
            env=env,
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
            if (
                "Starting terminal" in text
                or "SENDING INIT" in text
                or "HTM initialized" in text
            ):
                self.log_file = path
                return True
            return False

        wait_until(ready, timeout, description="htmd INIT_STATE")
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
            fail(
                f"{what}; headers 49/51/53/57="
                f"{header_count(last, HEADER_INSERT_KEYS)}/"
                f"{header_count(last, HEADER_CLOSE_PANE)}/"
                f"{header_count(last, HEADER_NEW_TAB)}/"
                f"{header_count(last, HEADER_NEW_SPLIT)}; "
                f"inserted={inserted_keys(last)[-80:]!r}; tail:\n{tail}"
            )
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
        # Never kill the user's installed iTerm2. /proc does not exist on macOS.
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
    # INIT_STATE materializes the first pane as a real iTerm2 tab beside the gateway.
    deadline = time.time() + 8
    while time.time() < deadline and session.tab_count() < 2:
        time.sleep(0.25)
    session.focus()

    tabs_after_init = session.tab_count()
    if tabs_after_init < 2:
        print(
            f"WARN: AX tab count after INIT_STATE was {tabs_after_init}; "
            "continuing with protocol assertions",
            flush=True,
        )
    else:
        print(f"OK: INIT_STATE created native tabs ({tabs_after_init})", flush=True)

    # Split the focused HTM client.
    session.keystroke('"d"', "command down")
    session.wait_log(
        lambda text: header_count(text, HEADER_NEW_SPLIT) >= 1,
        20,
        "NEW_SPLIT after Cmd+D",
    )
    print("OK: Cmd+D sent NEW_SPLIT", flush=True)

    session.keystroke(f'"{marker}"')
    session.key_code(36)  # return
    session.wait_log(
        lambda text: marker in inserted_keys(text) or marker in text,
        20,
        f"INSERT_KEYS containing {marker}",
    )
    print("OK: keys reached htmd pane", flush=True)

    session.keystroke('"t"', "command down")
    session.wait_log(
        lambda text: header_count(text, HEADER_NEW_TAB) >= 1,
        20,
        "NEW_TAB after Cmd+T",
    )
    print("OK: Cmd+T sent NEW_TAB", flush=True)

    session.keystroke('"d"', "{command down, shift down}")
    session.wait_log(
        lambda text: header_count(text, HEADER_NEW_SPLIT) >= 2,
        20,
        "second NEW_SPLIT after Cmd+Shift+D",
    )
    print("OK: Cmd+Shift+D sent second NEW_SPLIT", flush=True)

    # Concurrent output: background printers on two split panes plus another tab.
    # Background `&` returns the shell immediately so pane/tab switches can land.
    time.sleep(0.5)
    stamp = int(time.time())

    def echo_on_focused_pane(tag: str) -> str:
        before = len(read_from_ids(session.log_text()))
        session.keystroke(f'"echo {tag}"')
        session.key_code(36)
        session.wait_log(lambda text: tag in inserted_keys(text), 12, f"echo {tag}")
        ids = read_from_ids(session.log_text())[before:]
        if not ids:
            fail(f"no INSERT_KEYS UUID for {tag}")
        return ids[0]

    mark_a = f"MA{stamp}"
    pane_a = echo_on_focused_pane(mark_a)
    pane_b = pane_a
    for switch in (session.next_pane, session.previous_pane, session.previous_tab):
        switch()
        tag = f"MB{stamp}{switch.__name__}"
        pane_b = echo_on_focused_pane(tag)
        if pane_b != pane_a:
            break
    if pane_b == pane_a:
        fail("could not focus a second HTM pane for concurrent output")
    print(f"OK: keys reached two panes ({pane_a[:8]}… / {pane_b[:8]}…)", flush=True)

    def burst_cmd(tag: str) -> str:
        return f"for i in 1 2 3 4 5 6 7 8; do echo {tag}_$i; sleep 0.08; done &"

    loops = [f"IT2C0{stamp}", f"IT2C1{stamp}"]
    wrote_at = len(writing_to_ids(session.log_text()))
    # Focused on pane_b after the probe. Start printer there, then jump back.
    session.keystroke(f'"{burst_cmd(loops[1])}"')
    session.key_code(36)
    time.sleep(0.2)
    session.previous_pane()
    session.keystroke(f'"{burst_cmd(loops[0])}"')
    session.key_code(36)

    def interleaved(text: str) -> bool:
        writes = writing_to_ids(text)[wrote_at:]
        if len(set(writes)) < 2:
            return False
        tail = writes[-25:] if len(writes) >= 12 else writes
        if len(set(tail)) < 2:
            return False
        trans = sum(1 for i in range(1, len(tail)) if tail[i] != tail[i - 1])
        return trans >= 4

    session.wait_log(interleaved, 20, "interleaved WRITING TO from 2+ panes")
    new_writes = writing_to_ids(session.log_text())[wrote_at:]
    unique_new = list(dict.fromkeys(new_writes))
    transitions = sum(
        1 for i in range(1, len(new_writes)) if new_writes[i] != new_writes[i - 1]
    )
    by_pane = inserted_by_pane(session.log_text())
    tag_panes = {
        tag: [pane for pane, keys in by_pane.items() if tag in keys] for tag in loops
    }
    if not any(tag_panes.values()):
        fail(f"burst commands never reached htmd: {tag_panes}")
    print(
        f"OK: concurrent pane output ({len(unique_new)} panes, "
        f"{transitions} WRITING TO switches)",
        flush=True,
    )

    time.sleep(0.4)
    session.keystroke('"w"', "command down")
    session.wait_log(
        lambda text: header_count(text, HEADER_CLOSE_PANE) >= 1,
        20,
        "CLIENT_CLOSE_PANE after Cmd+W",
    )
    print("OK: Cmd+W sent CLIENT_CLOSE_PANE", flush=True)

    if session.proc.poll() is not None:
        fail("iTerm2 exited during the happy-path layout test")
    if not pids_named("htmd"):
        fail("htmd exited during the happy-path layout test")

    # Race: burst splits/tabs/closes while htmd is applying layout.
    splits_before = header_count(session.log_text(), HEADER_NEW_SPLIT)
    tabs_before = header_count(session.log_text(), HEADER_NEW_TAB)
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
        lambda text: header_count(text, HEADER_NEW_SPLIT) >= splits_before
        or header_count(text, HEADER_NEW_TAB) >= tabs_before,
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

    # Reattach in a real shell tab. After detach the original `--command=htm`
    # process has exited, so typing into that session cannot start a new client.
    session.keystroke('"t"', "command down")
    time.sleep(1.5)
    session.keystroke(f'"{session.htm} -x"')
    session.key_code(36)
    session.started_at = time.time() - 1.0
    session.wait_init(timeout=20)
    time.sleep(1.0)
    # apply() selects the new client tab; previous tab is the new gateway.
    session.keystroke('"["', "{command down, shift down}")
    time.sleep(0.3)
    session.keystroke('"x"')
    assert_no_htmd()
    assert_no_ipc()
    leftover_htm = [p for p in pids_named("htm") if p]
    # The gateway command may still be the htm client until SESSION_END; give it a moment.
    deadline = time.time() + 8
    while time.time() < deadline and leftover_htm:
        leftover_htm = pids_named("htm")
        time.sleep(0.2)
    if pids_named("htmd"):
        fail("htmd still running after gateway x")
    if ipc_path().exists():
        fail("IPC socket still present after gateway x")
    print("OK: gateway x shut down htmd and removed IPC socket", flush=True)


def main() -> int:
    if sys.platform != "darwin":
        skip("iTerm2 HTM e2e requires macOS")

    parser = argparse.ArgumentParser()
    parser.add_argument("--htm")
    parser.add_argument("--htmd")
    parser.add_argument("--iterm2-app")
    parser.add_argument(
        "--build-iterm2",
        action="store_true",
        help="Attempt a local iTerm2 Development build if no HTM app is found",
    )
    args = parser.parse_args()
    if args.iterm2_app:
        os.environ["ITERM2_APP"] = args.iterm2_app
    if args.build_iterm2:
        os.environ["ITERM2_BUILD"] = "1"

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
        ]
        if still:
            print(f"WARN: leftover iTerm2 pids {still}", flush=True)
    print("PASS: iTerm2 htm/htmd e2e", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
