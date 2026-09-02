#!/usr/bin/env python3
"""Shared GUI HTM e2e framework.

Tests (layout, stress) are emulator-agnostic. Each terminal plugs in by
implementing ``GuiTerminalSession`` and exporting ``NAME``, ``add_arguments``,
``apply_args``, and ``open_session``. Run one emulator:

  python3 test/system_tests/htm_gui_e2e.py --emulator ghostty --suite all
  python3 test/system_tests/hyper_htm_e2e.py --suite stress

Protocol checks read htmd ``control command:`` logs.
"""

from __future__ import annotations

import argparse
import importlib
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable, Optional, Sequence

SKIP = 77


def skip(reason: str) -> None:
    print(f"SKIP: {reason}", flush=True)
    raise SystemExit(SKIP)


def fail(reason: str) -> None:
    print(f"FAIL: {reason}", flush=True)
    raise SystemExit(1)


def applescript_quote(s: str) -> str:
    return '"' + str(s).replace('"', '""') + '"'


def typed_from_log(text: str) -> str:
    """Rebuild keystrokes from htmd ``control command: send`` log lines.

    Clients send printable characters as one ``send -lt %pane CHAR`` command
    each (or hex ``send -H``), so a marker never appears as a contiguous
    substring in the log unless we concatenate those payloads.
    """
    out: list[str] = []
    for cmd in control_commands(text):
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


def command_count(text: str, name: str) -> int:
    return sum(1 for cmd in control_commands(text) if cmd == name or cmd.startswith(f"{name} "))


def send_pane_ids(text: str) -> list[str]:
    ids: list[str] = []
    for cmd in control_commands(text):
        if not cmd.startswith("send"):
            continue
        tokens = cmd.split()
        i = 1
        while i < len(tokens):
            tok = tokens[i]
            if tok.startswith("-") and len(tok) > 1 and "t" in tok[1:]:
                if i + 1 < len(tokens) and tokens[i + 1][:1] == "%":
                    pane = tokens[i + 1][1:]
                    if pane.isdigit():
                        ids.append(pane)
                    i += 2
                    continue
            i += 1
    return ids


def unique_preserve(items: list[str]) -> list[str]:
    seen: set[str] = set()
    out: list[str] = []
    for item in items:
        if item in seen:
            continue
        seen.add(item)
        out.append(item)
    return out


def uid() -> int:
    return os.getuid() if os.name != "nt" else 0


def ipc_path() -> Path:
    if os.name == "nt":
        user = os.environ.get("USERNAME", "user")
        user = "".join(c if c.isalnum() or c in "_-" else "_" for c in user)
        return Path.cwd() / f"htm.{user or 'user'}.ipc"
    return Path("/tmp") / f"htm.{uid()}.ipc"


def list_htmd_logs() -> list[Path]:
    tmp = Path(tempfile.gettempdir()) if os.name == "nt" else Path("/tmp")
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


def log_created_at(path: Path) -> float:
    """Parse the timestamp encoded in ``htmd-YYYY-MM-DD_HH-MM-SS…log``.

    Filename time is more reliable than mtime when leftover logs are touched.
    """
    match = re.match(
        r"htmd-(\d{4})-(\d{2})-(\d{2})_(\d{2})-(\d{2})-(\d{2})",
        path.name,
    )
    if match:
        y, mo, d, h, mi, s = (int(g) for g in match.groups())
        try:
            return time.mktime((y, mo, d, h, mi, s, -1, -1, -1))
        except OverflowError:
            return 0.0
    try:
        return path.stat().st_mtime
    except OSError:
        return 0.0


def newest_log(logs: list[Path], started_at: float = 0.0) -> Optional[Path]:
    best = None
    best_score = 0.0
    for path in logs:
        created = log_created_at(path)
        try:
            mtime = path.stat().st_mtime
        except OSError:
            mtime = 0.0
        # Initial attach must never pin an older daemon log merely because its
        # mtime changed during cleanup. Reattach keeps using ``log_file`` and
        # does not need old files admitted here.
        if created < started_at:
            continue
        score = max(created, mtime)
        if score >= best_score:
            best_score = score
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
    if os.name == "nt":
        image = name if name.lower().endswith(".exe") else f"{name}.exe"
        try:
            output = subprocess.check_output(
                ["tasklist", "/FI", f"IMAGENAME eq {image}", "/FO", "CSV", "/NH"],
                text=True,
                stderr=subprocess.DEVNULL,
            )
        except (OSError, subprocess.CalledProcessError):
            return []
        import csv

        return [int(row[1]) for row in csv.reader(output.splitlines())
                if len(row) > 1 and row[0].casefold() == image.casefold()]
    try:
        out = subprocess.check_output(
            ["pgrep", "-x", "-U", str(uid()), name],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except FileNotFoundError:
        return []
    except subprocess.CalledProcessError:
        return []
    pids = []
    for line in out.split():
        try:
            pids.append(int(line))
        except ValueError:
            continue
    return pids


def process_is_running(name: str) -> bool:
    try:
        subprocess.check_call(
            ["pgrep", "-x", name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return True
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False


def kill_named(name: str, sig: int = signal.SIGTERM) -> None:
    if os.name == "nt":
        for pid in pids_named(name):
            subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return
    for pid in pids_named(name):
        try:
            os.kill(pid, sig)
        except OSError:
            pass


def kill_htm_daemons() -> None:
    kill_named("htmd")
    kill_named("htm")


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
    executable = "htm.exe" if os.name == "nt" else "htm"
    for candidate in (
        Path(__file__).resolve().parents[2] / "build" / "Release" / executable,
        Path(__file__).resolve().parents[2] / "build" / executable,
        Path.cwd() / executable,
        Path.cwd() / "build" / executable,
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
    sibling = htm.parent / ("htmd.exe" if os.name == "nt" else "htmd")
    if sibling.is_file():
        return sibling.resolve()
    skip("htmd binary is not built")


def assert_no_htmd() -> None:
    wait_until(
        lambda: not pids_named("htmd"),
        8,
        description="htmd process exit",
    )


def assert_no_ipc() -> None:
    wait_until(lambda: not ipc_path().exists(), 8, description="IPC socket removal")


class GuiHtmLogSession:
    """Pin to the htmd log created for this attach and wait on it."""

    def __init__(self, htm: Path, htmd: Path):
        self.htm = htm
        self.htmd = htmd
        self.log_file: Optional[Path] = None
        self.started_at = 0.0

    def log_text(self) -> str:
        return read_text(self.log_file)

    def wait_init(self, timeout: float = 25.0) -> str:
        def ready() -> bool:
            path = newest_log(list_htmd_logs(), self.started_at)
            text = read_text(path)
            if not path:
                return False
            if (
                "control command:" in text
                or "control-mode" in text
                or "Connected to endpoint" in text
                or "accepted, returned client_sock" in text
            ):
                self.log_file = path
                return True
            return False

        try:
            wait_until(ready, timeout, description="htmd control-mode attach")
        except SystemExit:
            path = newest_log(list_htmd_logs(), self.started_at)
            text = read_text(path)
            fail(
                "htmd control-mode attach; "
                f"htm={pids_named('htm')} htmd={pids_named('htmd')} "
                f"log={path} head:\n{text[:1500]}"
            )
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
            fail(
                f"{what}; "
                f"split-window={command_count(last, 'split-window')} "
                f"new-window={command_count(last, 'new-window')} "
                f"kill-pane={command_count(last, 'kill-pane')} "
                f"refresh-client={command_count(last, 'refresh-client')} "
                f"sendPanes={','.join(unique_preserve(send_pane_ids(last)))} "
                f"typed={typed_from_log(last)[-120:]!r}; "
                f"tail:\n{last[-2000:]}"
            )
        return last


class GuiTerminalSession(GuiHtmLogSession):
    """Plug-in interface for a GUI terminal that speaks tmux -CC against htm."""

    name = "terminal"
    require_kill_pane = True

    def start(self, command: str = "") -> None:
        raise NotImplementedError

    def stop(self) -> None:
        raise NotImplementedError

    def focus(self) -> None:
        raise NotImplementedError

    def keystroke(self, keys: str, using: str = "") -> None:
        raise NotImplementedError

    def key_code(self, code: int, using: str = "") -> None:
        raise NotImplementedError

    def previous_pane(self) -> None:
        self.keystroke('"["', "command down")
        time.sleep(0.35)

    def next_pane(self) -> None:
        self.keystroke('"]"', "command down")
        time.sleep(0.35)

    def previous_tab(self) -> None:
        self.keystroke('"["', "{command down, shift down}")
        time.sleep(0.4)

    def tab_count(self) -> int:
        return 0

    def after_attach(self) -> None:
        """Optional: focus the tmux-domain tab/window after control-mode attach."""

    def after_first_split(self) -> None:
        """Optional: emulator-only actions after the first Cmd+D split."""

    def after_marker(self, marker: str) -> None:
        """Optional: assert visible contents after typing ``marker``."""

    def after_layout_suite(self) -> None:
        """Optional: emulator-only checks after the shared layout suite."""

    def is_alive(self) -> bool:
        proc = getattr(self, "proc", None)
        if proc is None:
            return True
        return proc.poll() is None

    def warn_leftovers(self) -> None:
        """Optional: print leftover GUI pids after stop()."""


def _wait_for_native_tab(session: GuiTerminalSession) -> None:
    deadline = time.time() + 8
    while time.time() < deadline and session.tab_count() < 2:
        time.sleep(0.25)
    session.focus()


def _assert_session_alive(session: GuiTerminalSession, when: str) -> None:
    if not session.is_alive():
        fail(f"{session.name} exited {when}")
    if not pids_named("htmd"):
        fail(f"htmd exited {when}")


def run_gui_layout_io_tests(session: GuiTerminalSession) -> None:
    """Shared split/tab/keystroke/concurrent-I/O checks for every GUI driver."""
    session.start(f"{session.htm} -x")
    session.wait_init()
    session.after_attach()
    _wait_for_native_tab(session)
    print(f"OK: attached to {session.name}", flush=True)

    splits_before = command_count(session.log_text(), "split-window")
    session.keystroke('"d"', "command down")
    session.wait_log(
        lambda text: command_count(text, "split-window") > splits_before,
        20,
        "split-window after Cmd+D",
    )
    print("OK: Cmd+D sent split-window", flush=True)
    session.after_first_split()

    marker = f"HTM_E2E_{int(time.time())}"
    session.keystroke(f'"{marker}"')
    session.key_code(36)
    session.wait_log(
        lambda text: log_has_typed(text, marker),
        20,
        f"send-keys containing {marker}",
    )
    print("OK: keys reached htmd pane", flush=True)
    session.after_marker(marker)

    tabs_before = command_count(session.log_text(), "new-window")
    session.keystroke('"t"', "command down")
    session.wait_log(
        lambda text: command_count(text, "new-window") > tabs_before,
        20,
        "new-window after Cmd+T",
    )
    print("OK: Cmd+T sent new-window", flush=True)

    splits_before = command_count(session.log_text(), "split-window")
    session.keystroke('"d"', "{command down, shift down}")
    session.wait_log(
        lambda text: command_count(text, "split-window") > splits_before,
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

    loops = [f"GUI0{stamp}", f"GUI1{stamp}"]
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
    if session.require_kill_pane:
        session.wait_log(
            lambda text: "kill-pane" in text or "kill-window" in text,
            20,
            "CLIENT_CLOSE_PANE after Cmd+W",
        )
        print("OK: Cmd+W sent kill-pane/kill-window", flush=True)
    else:
        time.sleep(0.5)
        print("OK: Cmd+W delivered (kill-pane not required)", flush=True)

    _assert_session_alive(session, "during the happy-path layout test")

    splits_before = session.log_text().count("split-window")
    tabs_before = session.log_text().count("new-window")
    for _ in range(4):
        session.keystroke('"d"', "command down")
        session.keystroke('"t"', "command down")
        session.keystroke('"d"', "{command down, shift down}")
        session.keystroke('"w"', "command down")
    time.sleep(1.0)
    _assert_session_alive(session, "during rapid split/tab/close")
    session.wait_log(
        lambda text: text.count("split-window") >= splits_before
        or text.count("new-window") >= tabs_before,
        15,
        "htmd still accepting packets after race burst",
    )
    print(
        f"OK: rapid split/tab/close did not crash {session.name} or htmd",
        flush=True,
    )
    session.after_layout_suite()


def run_gui_stress(session: GuiTerminalSession) -> None:
    """Shared bulk-I/O stress: two panes printing while keys still flow."""
    session.start(f"{session.htm} -x")
    session.wait_init()
    session.after_attach()
    _wait_for_native_tab(session)
    print("OK: attached", flush=True)

    splits_before = command_count(session.log_text(), "split-window")
    session.keystroke('"d"', "command down")
    session.wait_log(
        lambda text: command_count(text, "split-window") > splits_before,
        20,
        "NEW_SPLIT after Cmd+D",
    )
    tabs_before = command_count(session.log_text(), "new-window")
    session.keystroke('"t"', "command down")
    session.wait_log(
        lambda text: command_count(text, "new-window") > tabs_before,
        20,
        "NEW_TAB after Cmd+T",
    )
    splits_before = command_count(session.log_text(), "split-window")
    session.keystroke('"d"', "{command down, shift down}")
    session.wait_log(
        lambda text: command_count(text, "split-window") > splits_before,
        20,
        "second NEW_SPLIT",
    )
    time.sleep(0.5)
    print("OK: tabs and splits created", flush=True)

    stamp = int(time.time())
    mark_a = f"STA{stamp}"
    mark_b = f"STB{stamp}"

    def echo_tag(tag: str) -> str:
        session.keystroke(f'"echo {tag}"')
        session.key_code(36)
        session.wait_log(lambda text: log_has_typed(text, tag), 12, f"echo {tag}")
        return tag

    pane_a = echo_tag(mark_a)
    pane_b = pane_a
    for switch in (session.next_pane, session.previous_pane, session.previous_tab):
        switch()
        pane_b = echo_tag(f"{mark_b}{switch.__name__}")
        if pane_b != pane_a:
            break
    if pane_b == pane_a:
        fail("could not focus a second HTM pane")
    print(f"OK: two panes {pane_a[:8]}… / {pane_b[:8]}…", flush=True)

    session.keystroke('"yes STBULK1 &"')
    session.key_code(36)
    time.sleep(0.25)

    switched = False
    for switch in (session.previous_pane, session.next_pane, session.previous_tab):
        switch()
        uid_tag = echo_tag(f"SW{stamp}{switch.__name__}")
        if uid_tag != pane_b:
            switched = True
            break
    if not switched:
        fail("could not move off the first bulk pane before starting the second")
    session.keystroke('"yes STBULK0 &"')
    session.key_code(36)
    time.sleep(0.3)
    for i in range(8):
        session.keystroke(f'"echo STKEY{stamp}_{i}"')
        session.key_code(36)
        time.sleep(0.08)

    session.wait_log(
        lambda text: text.count("control command: send") >= 4
        and log_has_typed(text, "STBULK"),
        25,
        "bulk send-keys while printers run",
    )
    _assert_session_alive(session, "during bulk I/O")
    print("OK: concurrent bulk I/O", flush=True)


def _shutdown_htmd() -> None:
    kill_named("htmd")
    assert_no_htmd()
    assert_no_ipc()
    leftover_htm = pids_named("htm")
    deadline = time.time() + 8
    while time.time() < deadline and leftover_htm:
        leftover_htm = pids_named("htm")
        time.sleep(0.2)
    if pids_named("htmd"):
        fail("htmd still running after SIGTERM")
    if ipc_path().exists():
        fail("IPC socket still present after htmd exit")
    print("OK: htmd shutdown removed IPC socket", flush=True)


def run_layout_suite(session: GuiTerminalSession) -> None:
    run_gui_layout_io_tests(session)
    _assert_session_alive(session, "after layout test")
    _shutdown_htmd()


def run_stress_suite(session: GuiTerminalSession) -> None:
    run_gui_stress(session)
    _shutdown_htmd()


SUITES: dict[str, Callable[[GuiTerminalSession], None]] = {
    "layout": run_layout_suite,
    "stress": run_stress_suite,
}
SUITE_ORDER = ("layout", "stress")

EMULATOR_MODULES = {
    "iterm2": "iterm2_htm_e2e",
    "hyper": "hyper_htm_e2e",
    "wezterm": "wezterm_htm_e2e",
    "ghostty": "ghostty_htm_e2e",
    "windows-terminal": "windows_terminal_htm_e2e",
}


def parse_suites(value: str) -> list[str]:
    raw = value.strip().lower()
    if raw in ("all", "*"):
        return list(SUITE_ORDER)
    names = [part.strip() for part in raw.replace(",", " ").split() if part.strip()]
    if not names:
        fail("no suites requested")
    unknown = [name for name in names if name not in SUITES]
    if unknown:
        fail(
            f"unknown suite(s) {unknown}; choose from "
            f"{', '.join(SUITE_ORDER)} or all"
        )
    return names


def run_gui_suites(
    session: GuiTerminalSession,
    suites: Sequence[str] = SUITE_ORDER,
) -> None:
    """Run emulator-agnostic suites against an already-constructed session."""
    for index, name in enumerate(suites):
        runner = SUITES.get(name)
        if runner is None:
            fail(f"unknown suite {name}")
        if index > 0:
            session.stop()
        print(f"== {session.name} / {name} ==", flush=True)
        runner(session)


def add_common_gui_args(parser: argparse.ArgumentParser, default_suite: str) -> None:
    parser.add_argument("--htm")
    parser.add_argument("--htmd")
    parser.add_argument(
        "--suite",
        default=default_suite,
        help="layout, stress, comma-separated names, or all "
        f"(default: {default_suite})",
    )


def run_emulator_main(module: object, default_suite: str = "layout") -> int:
    """CLI entry used by per-emulator scripts and the unified runner."""
    name = getattr(module, "NAME", "GUI")
    platforms = getattr(module, "PLATFORMS", ("darwin",))
    if sys.platform not in platforms:
        skip(f"{name} HTM e2e requires macOS")

    parser = argparse.ArgumentParser()
    add_common_gui_args(parser, default_suite=default_suite)
    add_arguments = getattr(module, "add_arguments", None)
    if callable(add_arguments):
        add_arguments(parser)
    args = parser.parse_args()
    apply_args = getattr(module, "apply_args", None)
    if callable(apply_args):
        apply_args(args)

    htm = find_htm_bin(args.htm)
    htmd = find_htmd_bin(args.htmd, htm)
    open_session = getattr(module, "open_session")
    session: GuiTerminalSession = open_session(htm, htmd, args)
    print(f"Using {session.name}={getattr(session, 'app', session.name)}", flush=True)
    print(f"Using htm={htm}", flush=True)
    print(f"Using htmd={htmd}", flush=True)

    suites = parse_suites(args.suite)
    try:
        run_gui_suites(session, suites)
    finally:
        session.stop()
        kill_named("htmd")
        session.warn_leftovers()
    print(f"PASS: {session.name} htm/htmd e2e ({', '.join(suites)})", flush=True)
    return 0


def _peek_option(argv: Sequence[str], name: str) -> Optional[str]:
    flag = f"--{name}"
    for index, arg in enumerate(argv):
        if arg == flag and index + 1 < len(argv):
            return argv[index + 1]
        prefix = f"{flag}="
        if arg.startswith(prefix):
            return arg[len(prefix) :]
    return None


def main() -> int:
    emulator = _peek_option(sys.argv[1:], "emulator")
    parser = argparse.ArgumentParser(
        description="Run shared HTM GUI e2e suites against a plugged-in emulator"
    )
    parser.add_argument(
        "--emulator",
        required=True,
        choices=sorted(EMULATOR_MODULES),
        help="terminal driver to launch",
    )
    add_common_gui_args(parser, default_suite="all")
    if emulator and emulator in EMULATOR_MODULES:
        module = importlib.import_module(EMULATOR_MODULES[emulator])
        add_arguments = getattr(module, "add_arguments", None)
        if callable(add_arguments):
            add_arguments(parser)
    else:
        module = None
    args = parser.parse_args()
    if module is None:
        module = importlib.import_module(EMULATOR_MODULES[args.emulator])
    apply_args = getattr(module, "apply_args", None)
    if callable(apply_args):
        apply_args(args)
    platforms = getattr(module, "PLATFORMS", ("darwin",))
    if sys.platform not in platforms:
        skip(f"{args.emulator} HTM e2e requires macOS")

    htm = find_htm_bin(args.htm)
    htmd = find_htmd_bin(args.htmd, htm)
    session: GuiTerminalSession = module.open_session(htm, htmd, args)
    print(f"Using {session.name}={getattr(session, 'app', session.name)}", flush=True)
    print(f"Using htm={htm}", flush=True)
    print(f"Using htmd={htmd}", flush=True)
    suites = parse_suites(args.suite)
    try:
        run_gui_suites(session, suites)
    finally:
        session.stop()
        kill_named("htmd")
        session.warn_leftovers()
    print(f"PASS: {session.name} htm/htmd e2e ({', '.join(suites)})", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
