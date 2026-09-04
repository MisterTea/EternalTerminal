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
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable, Optional, Sequence

import htm_gui_parity
import iterm2_tmux_cc_oracle as tmux_cc

SKIP = 77

_NATIVE_MUX_TITLE = re.compile(r" \[(?:tmux|htm|@[^]]+)\]\s*$")


def _is_gateway_title(name: str) -> bool:
    """iTerm2's tmux -CC gateway is titled like ``[↣ tmux tmux]`` / ``[↣ htm htm]``."""
    n = name or ""
    lower = n.lower()
    if "tmux tmux" in lower or "htm htm" in lower:
        return True
    if n.startswith("[") and not _NATIVE_MUX_TITLE.search(n):
        return "tmux" in lower or "htm" in lower
    return False


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

    Hex payloads are UTF-8 bytes (possibly several ``0xNN`` tokens per
    command for one codepoint). Decode them as UTF-8, not Latin-1.
    """
    out: list[str] = []
    byte_buf = bytearray()

    def flush_bytes() -> None:
        nonlocal byte_buf
        if byte_buf:
            out.append(bytes(byte_buf).decode("utf-8", errors="replace"))
            byte_buf = bytearray()

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
                byte_buf.append(int(item, 16) & 0xFF)
            elif hex_mode and re.fullmatch(r"[0-9A-Fa-f]{1,2}", item):
                byte_buf.append(int(item, 16) & 0xFF)
            else:
                flush_bytes()
                out.append(item)
        flush_bytes()
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


def pane_dump_path() -> Path:
    if os.name == "nt":
        user = os.environ.get("USERNAME", "user")
        user = "".join(c if c.isalnum() or c in "_-" else "_" for c in user)
        return Path(tempfile.gettempdir()) / f"htm.{user or 'user'}.panes"
    return ipc_path().with_suffix(".panes")


def request_htmd_pane_dump() -> bool:
    """Ask htmd to write its pane dump via the Windows named event."""
    if os.name != "nt":
        return False
    import ctypes
    from ctypes import wintypes

    user = os.environ.get("USERNAME", "user")
    user = "".join(c if c.isalnum() or c in "_-" else "_" for c in user) or "user"
    name = f"Local\\EternalTerminal.HtmPaneDump.{user}".encode("ascii", "ignore")
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateEventA.restype = wintypes.HANDLE
    kernel32.CreateEventA.argtypes = [
        ctypes.c_void_p,
        wintypes.BOOL,
        wintypes.BOOL,
        wintypes.LPCSTR,
    ]
    handle = kernel32.CreateEventA(None, True, False, name)
    if not handle:
        return False
    try:
        return bool(kernel32.SetEvent(handle))
    finally:
        kernel32.CloseHandle(handle)


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
            subprocess.run(
                ["taskkill", "/PID", str(pid), "/T"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        deadline = time.time() + 2
        while time.time() < deadline and pids_named(name):
            time.sleep(0.1)
        for pid in pids_named(name):
            subprocess.run(
                ["taskkill", "/PID", str(pid), "/T", "/F"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
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


def find_tmux_bin() -> Path:
    env = os.environ.get("TMUX_BIN")
    if env and Path(env).is_file():
        return Path(env).resolve()
    for candidate in (
        Path("/opt/homebrew/bin/tmux"),
        Path("/usr/local/bin/tmux"),
        Path("/usr/bin/tmux"),
    ):
        if candidate.is_file():
            return candidate.resolve()
    try:
        out = subprocess.check_output(["which", "tmux"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        out = ""
    if out and Path(out).is_file():
        return Path(out).resolve()
    skip("tmux is not installed; needed for --mux tmux")


def muxes_for_suites(muxes: list[str], suites: Sequence[str]) -> list[str]:
    """Corners diffs htm against iTerm2+tmux -CC (live tmux on Unix)."""
    if "corners" not in suites:
        return muxes
    if os.name == "nt":
        print(
            "corners on Windows uses htm vs the iTerm2+tmux -CC oracle "
            "(no local tmux -CC)",
            flush=True,
        )
        return ["htm"]
    if muxes != ["tmux", "htm"]:
        print(
            "corners verifies htm against iTerm2+tmux -CC; using --mux both",
            flush=True,
        )
    return ["tmux", "htm"]


def verify_gui_parity_against_tmux_cc(
    emulator: str, text_dir: Path, muxes: Sequence[str], suites: Sequence[str]
) -> None:
    if list(muxes) != ["tmux", "htm"]:
        return
    slug = re.sub(r"[^a-z0-9]+", "-", emulator.lower()).strip("-")
    for suite in suites:
        tmux_dir = text_dir / f"{slug}-tmux-{suite}-steps"
        htm_dir = text_dir / f"{slug}-htm-{suite}-steps"
        if not tmux_dir.is_dir() or not htm_dir.is_dir():
            fail(
                f"{suite} missing tmux -CC vs htm snapshots: "
                f"tmux={tmux_dir} htm={htm_dir}"
            )
        verdicts = htm_gui_parity.compare_step_dirs(tmux_dir, htm_dir)
        print(f"== {suite} tmux -CC vs htm parity ==", flush=True)
        print(htm_gui_parity.format_verdicts(verdicts), flush=True)
        bad = htm_gui_parity.divergences(verdicts)
        if bad:
            detail = "\n".join(
                f"{item['action']}: {item['detail']}" for item in bad
            )
            fail(
                f"htm {suite} snapshots diverged from tmux -CC "
                f"(not cosmetic/timing):\n{detail}"
            )
        print(f"OK: htm {suite} snapshots match tmux -CC", flush=True)


def parse_mux(value: str) -> list[str]:
    raw = (value or "htm").strip().lower()
    if raw in ("both", "all"):
        return ["tmux", "htm"]
    if raw in ("htm", "tmux"):
        return [raw]
    fail("--mux must be htm, tmux, or both")


def assert_no_htmd() -> None:
    wait_until(
        lambda: not pids_named("htmd"),
        8,
        description="htmd process exit",
    )


def assert_no_ipc() -> None:
    def ipc_gone() -> bool:
        path = ipc_path()
        if not path.exists():
            return True
        if os.name == "nt" and not pids_named("htmd"):
            try:
                path.unlink()
            except OSError:
                pass
            return not path.exists()
        return False

    wait_until(ipc_gone, 8, description="IPC socket removal")


class GuiHtmLogSession:
    """Pin to the htmd log created for this attach and wait on it."""

    def __init__(self, htm: Path, htmd: Path):
        self.htm = htm
        self.htmd = htmd
        self.log_file: Optional[Path] = None
        self.started_at = 0.0
        self.mux = "htm"
        self.tmux_bin: Optional[Path] = None
        self.tmux_socket = ""

    def log_text(self) -> str:
        return read_text(self.log_file)

    def wait_init(self, timeout: float = 25.0) -> str:
        if self.mux == "tmux":
            wait_until(
                self.tmux_has_session,
                timeout,
                description="tmux -CC session",
            )
            return ""
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


class ScreenRecorder:
    """Record one window rectangle (macOS ``screencapture -v``, Windows ffmpeg)."""

    def __init__(self, path: Path, region: tuple[int, int, int, int], label: str):
        self.path = path
        self.region = region
        self.label = label
        self.proc: Optional[subprocess.Popen] = None

    def start(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if self.path.exists():
            self.path.unlink()
        x, y, width, height = self.region
        rect = f"{x},{y},{width},{height}"
        print(f"recording {self.label} {rect} -> {self.path}", flush=True)
        if os.name == "nt":
            self._start_ffmpeg(x, y, width, height)
            return
        self.proc = subprocess.Popen(
            ["screencapture", "-v", "-x", "-R", rect, str(self.path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        time.sleep(0.4)
        if self.proc.poll() is not None:
            err = (self.proc.stderr.read() or b"").decode("utf-8", "replace")
            fail(f"screencapture -v failed for {rect}: {err.strip() or 'exit'}")

    def _start_ffmpeg(self, x: int, y: int, width: int, height: int) -> None:
        try:
            ctypes = __import__("ctypes")
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass
        width = max(2, int(width) & ~1)
        height = max(2, int(height) & ~1)
        x = max(0, int(x))
        y = max(0, int(y))
        argv = [
            "ffmpeg",
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "gdigrab",
            "-framerate",
            "30",
            "-offset_x",
            str(x),
            "-offset_y",
            str(y),
            "-video_size",
            f"{width}x{height}",
            "-draw_mouse",
            "0",
            "-i",
            "desktop",
            "-c:v",
            "libx264",
            "-pix_fmt",
            "yuv420p",
            "-preset",
            "veryfast",
            str(self.path),
        ]
        self.proc = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        time.sleep(0.6)
        if self.proc.poll() is not None:
            err = (self.proc.stderr.read() or b"").decode("utf-8", "replace")
            fail(f"ffmpeg gdigrab failed for {x},{y} {width}x{height}: {err.strip() or 'exit'}")

    def stop(self, required: bool = True) -> None:
        if self.proc is None or self.proc.poll() is not None:
            self.proc = None
            return
        if os.name == "nt":
            try:
                if self.proc.stdin:
                    self.proc.stdin.write(b"q")
                    self.proc.stdin.close()
            except OSError:
                self.proc.terminate()
        else:
            self.proc.send_signal(signal.SIGINT)
        try:
            self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=3)
        self.proc = None
        size = self.path.stat().st_size if self.path.is_file() else 0
        if size < 1000:
            msg = f"video recording missing or tiny: {self.path}"
            if required:
                fail(msg)
            print(f"WARN: {msg}", flush=True)
            return
        print(f"saved video {self.path} ({size} bytes)", flush=True)


def _parse_ax_windows(raw: str) -> list[dict]:
    windows = []
    for line in raw.splitlines():
        parts = line.split("\t")
        if len(parts) < 6:
            continue
        try:
            win = {
                "index": int(parts[0]),
                "name": parts[1],
                "x": float(parts[2]),
                "y": float(parts[3]),
                "w": float(parts[4]),
                "h": float(parts[5]),
                "id": int(parts[6]) if len(parts) > 6 and parts[6].strip() else 0,
            }
        except ValueError:
            continue
        windows.append(win)
    return windows


def _frame_key(win: dict) -> tuple[int, int, int, int]:
    return (int(win["x"]), int(win["y"]), int(win["w"]), int(win["h"]))


def _window_key(win: dict) -> str:
    if win.get("id"):
        return f"id:{win['id']}"
    name = (win.get("name") or "").strip()
    frame = ",".join(str(v) for v in _frame_key(win))
    if name:
        return f"name:{name}|frame:{frame}"
    return f"frame:{frame}"


def _newest_native(windows: list[dict]) -> dict:
    return max(
        windows,
        key=lambda w: (int(w.get("id") or 0), int(w.get("index") or 0)),
    )


class GuiTerminalSession(GuiHtmLogSession):
    """Plug-in interface for a GUI terminal that speaks tmux -CC against htm."""

    name = "terminal"
    require_kill_pane = True
    ax_process_name: Optional[str] = None
    supports_detach = False
    supports_native_resize = False

    def __init__(self, htm: Path, htmd: Path):
        super().__init__(htm, htmd)
        self.video_dir: Optional[Path] = None
        self.text_dir: Optional[Path] = None
        self._gateway_keys: set[str] = set()
        self._gateway_names: set[str] = set()
        self._gateway_clicks: list[tuple[float, float]] = []
        self._saw_native_mux_windows = False
        self._recorders: dict[str, ScreenRecorder] = {}
        self._record_suite = "suite"
        self._next_win_n = 1
        self._text_n = 0
        self._capturing_text = False
        self._suite_started_at = 0.0
        self._step_dir: Optional[Path] = None
        self._front_native: Optional[dict] = None

    def multiplexer_command(self) -> str:
        if self.mux == "tmux":
            if not self.tmux_bin:
                skip("tmux is not installed; needed for --mux tmux")
            return (
                f"{self.tmux_bin} -L {self.tmux_socket} -f /dev/null "
                f"-CC new-session"
            )
        return f"{self.htm} -x"

    def tmux_argv(self, *args: str) -> list[str]:
        if not self.tmux_bin:
            skip("tmux is not installed; needed for --mux tmux")
        return [str(self.tmux_bin), "-L", self.tmux_socket, *args]

    def tmux_cmd(self, *args: str) -> str:
        try:
            return subprocess.check_output(
                self.tmux_argv(*args),
                text=True,
                stderr=subprocess.DEVNULL,
            )
        except (OSError, subprocess.CalledProcessError):
            return ""

    def tmux_has_session(self) -> bool:
        return bool(self.tmux_cmd("list-sessions"))

    def tmux_client_count(self) -> int:
        return len(
            [line for line in self.tmux_cmd("list-clients").splitlines() if line.strip()]
        )

    def tmux_pane_count(self) -> int:
        return len(
            [line for line in self.tmux_cmd("list-panes", "-a").splitlines() if line.strip()]
        )

    def tmux_window_count(self) -> int:
        return len(
            [line for line in self.tmux_cmd("list-windows", "-a").splitlines() if line.strip()]
        )

    def tmux_all_pane_text(self) -> str:
        ids = [
            line.strip()
            for line in self.tmux_cmd("list-panes", "-a", "-F", "#{pane_id}").splitlines()
            if line.strip()
        ]
        return "\n".join(self.tmux_cmd("capture-pane", "-p", "-J", "-t", pane) for pane in ids)

    def tmux_pane_snapshot(self) -> str:
        """Visible screen of every pane, matching tmux capture-pane -p -J."""
        rows = [
            line.split("\t")
            for line in self.tmux_cmd(
                "list-panes",
                "-a",
                "-F",
                "#{window_id}\t#{window_name}\t#{pane_id}\t#{pane_active}\t"
                "#{pane_width}x#{pane_height}\t#{cursor_x},#{cursor_y}\t"
                "#{pane_pid}\t#{window_active}",
            ).splitlines()
            if line.strip()
        ]
        chunks: list[str] = []
        for parts in rows:
            if len(parts) < 6:
                continue
            wid, name, pane, active, size, cursor = parts[:6]
            shell_pid = parts[6] if len(parts) > 6 else ""
            win_active = parts[7] if len(parts) > 7 else "0"
            text = self.tmux_cmd(
                "capture-pane", "-p", "-J", "-N", "-S", "-1000", "-t", pane
            )
            pid_field = f" shell_pid={shell_pid}" if shell_pid else ""
            current = (
                "1" if active == "1" and win_active == "1" else "0"
            )
            chunks.append(
                f"--- window {wid} name={name} pane {pane} active={active} "
                f"{size} cursor={cursor}{pid_field} current={current}\n{text}"
            )
            if text and not text.endswith("\n"):
                chunks[-1] += "\n"
        return "".join(chunks)

    def htm_pane_snapshot(self, wait: float = 2.0) -> str:
        """Ask htmd for the same visible-screen dump as capture-pane -p -J."""
        path = pane_dump_path()
        before = path.stat().st_mtime if path.is_file() else 0.0
        pids = pids_named("htmd")
        if not pids:
            return ""
        if os.name == "nt":
            dump_paths = [path]
            htmd = getattr(self, "htmd", None)
            if htmd:
                sibling = Path(htmd).parent / path.name
                if sibling not in dump_paths:
                    dump_paths.append(sibling)
            before_times = {
                p: (p.stat().st_mtime if p.is_file() else 0.0) for p in dump_paths
            }
            if not request_htmd_pane_dump():
                for p in dump_paths:
                    text = read_text(p) if p.is_file() else ""
                    if text:
                        return text
                return ""
            deadline = time.time() + wait
            while time.time() < deadline:
                for p in dump_paths:
                    try:
                        if p.is_file() and p.stat().st_mtime > before_times[p]:
                            return read_text(p)
                    except OSError:
                        pass
                time.sleep(0.05)
            for p in dump_paths:
                text = read_text(p) if p.is_file() else ""
                if text:
                    return text
            return ""
        for pid in pids:
            try:
                os.kill(pid, signal.SIGUSR1)
            except OSError:
                continue

        deadline = time.time() + wait
        while time.time() < deadline:
            try:
                if path.is_file() and path.stat().st_mtime > before:
                    return read_text(path)
            except OSError:
                pass
            time.sleep(0.05)
        return read_text(path) if path.is_file() else ""

    def mux_snapshot(self, wait: float = 2.0) -> str:
        if self.mux == "tmux":
            return self.tmux_pane_snapshot()
        return self.htm_pane_snapshot(wait=wait)

    def mux_pane_count(self) -> int:
        return len(htm_gui_parity.parse_panes(self.mux_snapshot(wait=0.4)))

    def mux_window_count(self) -> int:
        panes = htm_gui_parity.parse_panes(self.mux_snapshot(wait=0.4))
        return len({pane["wid"] for pane in panes})

    def mux_window_names(self) -> list[str]:
        return [
            htm_gui_parity.cosmetic_title(pane["name"])
            for pane in htm_gui_parity.parse_panes(self.mux_snapshot(wait=0.4))
        ]

    def wait_visible(self, marker: str, timeout: float = 20.0) -> None:
        wait_until(
            lambda: marker in self.mux_snapshot(wait=0.35),
            timeout,
            description=f"visible pane text contains {marker}",
        )

    def wait_mux_pane_count(self, count: int, timeout: float = 20.0) -> None:
        wait_until(
            lambda: self.mux_pane_count() == count,
            timeout,
            description=f"{count} live panes",
        )

    def wait_mux_window_count(self, count: int, timeout: float = 20.0) -> None:
        wait_until(
            lambda: self.mux_window_count() == count,
            timeout,
            description=f"{count} live windows",
        )

    def wait_window_named(self, name: str, timeout: float = 20.0) -> None:
        wait_until(
            lambda: name in self.mux_window_names(),
            timeout,
            description=f"window named {name}",
        )

    def checkpoint(self, step_id: str, *, oracle: bool = True) -> str:
        """Record this mux's dump and assert the iTerm2+tmux -CC oracle."""
        dump = self.mux_snapshot()
        body = f"# action: {step_id}\n# mux={self.mux}\n\n{dump}"
        if self._step_dir:
            self._step_dir.mkdir(parents=True, exist_ok=True)
            (self._step_dir / f"{step_id}.txt").write_text(body, encoding="utf-8")
        if oracle:
            errors = tmux_cc.check_step(step_id, dump)
            if errors:
                fail(
                    f"{self.mux} checkpoint {step_id} diverged from "
                    "iTerm2+tmux -CC: "
                    + "; ".join(errors)
                    + f"\n{dump[:2000]}"
                )
            print(
                f"OK: {step_id} matches iTerm2+tmux -CC oracle ({self.mux})",
                flush=True,
            )
        else:
            print(f"OK: recorded parity checkpoint {step_id} ({self.mux})", flush=True)
        return dump

    def emulator_window_snapshot(self) -> str:
        windows = self.ax_windows()
        if not windows:
            return ""
        lines = ["--- emulator windows ---"]
        for win in windows:
            lines.append(
                f"win{win['index']} {_window_key(win)} name={win.get('name') or ''} "
                f"frame={int(win['x'])},{int(win['y'])},{int(win['w'])},{int(win['h'])}"
            )
        return "\n".join(lines) + "\n"

    def snapshot_all_text(self, action: str) -> None:
        """Write every pane's visible text after a test action."""
        if self._capturing_text or not self.text_dir:
            return
        self._capturing_text = True
        try:
            if not self._suite_started_at:
                self._suite_started_at = time.time()
            self._text_n += 1
            slug = re.sub(r"[^a-zA-Z0-9]+", "-", action).strip("-")[:80] or "action"
            dest_dir = (
                self.text_dir
                / f"{re.sub(r'[^a-z0-9]+', '-', self.name.lower()).strip('-')}"
                f"-{self.mux}-{self._record_suite}-text"
            )
            dest_dir.mkdir(parents=True, exist_ok=True)
            elapsed = time.time() - self._suite_started_at
            if self.mux == "tmux":
                panes = self.tmux_pane_snapshot()
            else:
                panes = self.htm_pane_snapshot()
            body = (
                f"# action: {action}\n"
                f"# t={elapsed:.3f}s mux={self.mux} n={self._text_n}\n\n"
                f"{panes}"
            )
            path = dest_dir / f"{self._text_n:04d}-{slug}.txt"
            path.write_text(body, encoding="utf-8")
        except OSError as exc:
            print(f"WARN: pane text snapshot failed: {exc}", flush=True)
        finally:
            self._capturing_text = False

    def split_watermark(self) -> int:
        if self.mux == "tmux":
            return self.tmux_pane_count()
        return command_count(self.log_text(), "split-window")

    def window_watermark(self) -> int:
        if self.mux == "tmux":
            return self.tmux_window_count()
        return command_count(self.log_text(), "new-window")

    def wait_split(self, before: int) -> None:
        if self.mux == "tmux":
            wait_until(
                lambda: self.tmux_pane_count() > before,
                20,
                description="tmux pane after split",
            )
        else:
            self.wait_log(
                lambda text: command_count(text, "split-window") > before,
                20,
                "split-window",
            )
        self.snapshot_all_text("after-split")

    def wait_new_window(self, before: int) -> None:
        if self.mux == "tmux":
            wait_until(
                lambda: self.tmux_window_count() > before,
                20,
                description="tmux window after Cmd+T",
            )
        else:
            self.wait_log(
                lambda text: command_count(text, "new-window") > before,
                20,
                "new-window",
            )
        self.snapshot_all_text("after-new-window")

    def wait_typed(self, marker: str, timeout: float = 20.0) -> None:
        if self.mux == "tmux":
            wait_until(
                lambda: marker in self.tmux_all_pane_text(),
                timeout,
                description=f"tmux pane contains {marker}",
            )
        else:
            self.wait_log(
                lambda text: log_has_typed(text, marker),
                timeout,
                f"send-keys containing {marker}",
            )
        self.snapshot_all_text(f"after-typed-{marker}")

    def wait_kill_pane(self, before_panes: int) -> None:
        if self.mux == "tmux":
            wait_until(
                lambda: self.tmux_pane_count() < before_panes
                or self.tmux_window_count() < 1,
                20,
                description="tmux pane/window closed",
            )
        else:
            self.wait_log(
                lambda text: "kill-pane" in text or "kill-window" in text,
                20,
                "CLIENT_CLOSE_PANE after Cmd+W",
            )
        self.snapshot_all_text("after-kill-pane")

    def shutdown_multiplexer(self) -> None:
        if self.mux == "tmux":
            subprocess.run(
                self.tmux_argv("kill-server"),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
            wait_until(
                lambda: not self.tmux_has_session(),
                8,
                description="tmux server exit",
            )
            print("OK: tmux -CC server exited", flush=True)
            return
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

    def ax_tell_target(self) -> str:
        proc = getattr(self, "proc", None)
        if proc is not None and proc.poll() is None:
            pid = getattr(self, "pid", None)
            if isinstance(pid, int):
                return f"(first process whose unix id is {pid})"
        name = self.ax_process_name or self.name
        return f'process "{name}"'

    def ax_windows(self) -> list[dict]:
        script = f'''
tell application "System Events"
  tell {self.ax_tell_target()}
    set output to ""
    set i to 0
    repeat with w in windows
      set i to i + 1
      set p to position of w
      set s to size of w
      set n to ""
      try
        set n to name of w as text
      end try
      set wid to 0
      try
        set wid to id of w
      end try
      if wid is 0 then
        try
          set wid to value of attribute "AXWindowNumber" of w
        end try
      end if
      set output to output & i & tab & n & tab & (item 1 of p) & tab & (item 2 of p) & tab & (item 1 of s) & tab & (item 2 of s) & tab & wid & linefeed
    end repeat
    return output
  end tell
end tell
'''
        try:
            return _parse_ax_windows(run_osascript(script))
        except (subprocess.CalledProcessError, SystemExit):
            return []

    def remember_gateway_windows(self) -> None:
        """Snapshot the original terminal before HTM opens pane windows."""
        windows = self.ax_windows()
        non_native = [
            w
            for w in windows
            if not _NATIVE_MUX_TITLE.search(w.get("name") or "")
        ]
        commandish = [
            w
            for w in non_native
            if _is_gateway_title(w.get("name") or "")
        ]
        gateways = commandish or non_native or windows
        self._gateway_keys = {_window_key(w) for w in gateways}
        self._gateway_names = {(w.get("name") or "").strip() for w in gateways}
        self._gateway_names.discard("")
        self._gateway_clicks = [
            (float(w["x"]) + float(w["w"]) * 0.5, float(w["y"]) + float(w["h"]) * 0.45)
            for w in gateways
        ]
        details = [
            f"{_window_key(w)}:{w.get('name') or '(unnamed)'}" for w in gateways
        ]
        print(
            f"gateway windows: {len(self._gateway_keys)} {details}",
            flush=True,
        )

    def click_screen(self, x: float, y: float) -> None:
        """Optional: click global screen coordinates (iTerm2 implements this)."""

    def focus_native_window(self) -> None:
        """Raise a tmux -CC native pane window, not the gateway PTY."""
        launched = self.launched_windows()
        self.focus()
        if not launched:
            return
        win = _newest_native(launched)
        self._front_native = win
        self._raise_ax_window(win)

    def remember_front_native(self) -> None:
        launched = self.launched_windows()
        if launched:
            self._front_native = _newest_native(launched)

    def restore_front_native(self) -> None:
        saved = getattr(self, "_front_native", None)
        launched = self.launched_windows()
        win = None
        if saved and saved.get("id"):
            win = next((w for w in launched if w.get("id") == saved.get("id")), None)
        if win is None and saved:
            win = next(
                (
                    w
                    for w in launched
                    if (w.get("name") or "") == (saved.get("name") or "")
                    and _frame_key(w) == _frame_key(saved)
                ),
                None,
            )
        if win is None and launched:
            win = _newest_native(launched)
        if not win:
            self.focus()
            return
        self._front_native = win
        self._raise_ax_window(win)

    def _raise_ax_window(self, win: dict) -> None:
        script = f'''
tell application "System Events"
  tell {self.ax_tell_target()}
    set frontmost to true
    try
      perform action "AXRaise" of window {int(win["index"])}
    end try
    try
      set index of window {int(win["index"])} to 1
    end try
  end tell
end tell
'''
        try:
            run_osascript(script)
        except (subprocess.CalledProcessError, SystemExit):
            self.focus()
        time.sleep(0.2)
        self.click_screen(
            float(win["x"]) + float(win["w"]) * 0.72,
            float(win["y"]) + float(win["h"]) * 0.55,
        )
        time.sleep(0.15)

    def focus_gateway(self) -> None:
        """Raise the original --command session so Esc/detach reach the menu."""
        windows = self.ax_windows()
        targets = [
            w for w in windows if _is_gateway_title(w.get("name") or "")
        ]
        if not targets:
            targets = [w for w in windows if _window_key(w) in self._gateway_keys]
        if not targets and self._gateway_names:
            targets = [
                w
                for w in windows
                if (w.get("name") or "").strip() in self._gateway_names
            ]
        if not targets and self._gateway_clicks:
            gx, gy = self._gateway_clicks[0]
            for w in windows:
                if (
                    w["x"] <= gx <= w["x"] + w["w"]
                    and w["y"] <= gy <= w["y"] + w["h"]
                    and not _NATIVE_MUX_TITLE.search(w.get("name") or "")
                ):
                    targets = [w]
                    break
        if not targets:
            print(
                "WARN: gateway title not matched; windows="
                + str([(w.get("name"), w.get("index")) for w in windows]),
                flush=True,
            )
            targets = [
                w
                for w in windows
                if not _NATIVE_MUX_TITLE.search(w.get("name") or "")
            ] or windows[:1]
        if not targets:
            print("WARN: no gateway window to raise", flush=True)
            return
        target = targets[0]
        script = f'''
tell application "System Events"
  tell {self.ax_tell_target()}
    set frontmost to true
    try
      perform action "AXRaise" of window {int(target["index"])}
    end try
    try
      set index of window {int(target["index"])} to 1
    end try
  end tell
end tell
'''
        try:
            run_osascript(script)
        except (subprocess.CalledProcessError, SystemExit):
            print(
                f"WARN: failed to raise gateway window {target.get('name')}",
                flush=True,
            )
            return
        time.sleep(0.25)
        self.click_screen(
            float(target["x"]) + float(target["w"]) * 0.5,
            float(target["y"]) + float(target["h"]) * 0.45,
        )
        time.sleep(0.2)
        print(
            f"focused gateway: {target.get('name') or _window_key(target)}",
            flush=True,
        )

    def launched_windows(self) -> list[dict]:
        """Native windows HTM/tmux -CC opened, excluding the gateway PTY."""
        current = self.ax_windows()
        native = [
            w for w in current if _NATIVE_MUX_TITLE.search(w.get("name") or "")
        ]
        if native:
            self._saw_native_mux_windows = True
            return native
        if self._saw_native_mux_windows:
            return []
        launched = [w for w in current if _window_key(w) not in self._gateway_keys]
        if launched:
            return launched
        extra = len(current) - len(self._gateway_keys)
        if extra > 0:
            return current[:extra]
        return []

    def sync_htm_window_recordings(self) -> None:
        """Start a new file for each newly launched HTM window; stop closed ones."""
        if not self.video_dir:
            return
        launched = self.launched_windows()
        live = set()
        for win in launched:
            key = _window_key(win)
            live.add(key)
            if key in self._recorders:
                continue
            n = self._next_win_n
            self._next_win_n += 1
            slug = re.sub(r"[^a-z0-9]+", "-", self.name.lower()).strip("-")
            ext = ".mp4" if os.name == "nt" else ".mov"
            path = (
                self.video_dir
                / f"{slug}-{self.mux}-{self._record_suite}-win{n:02d}{ext}"
            )
            region = (int(win["x"]), int(win["y"]), int(win["w"]), int(win["h"]))
            label = win["name"] or f"window {win['index']}"
            rec = ScreenRecorder(path, region, f"{label} ({key})")
            rec.start()
            self._recorders[key] = rec
        for key in list(self._recorders):
            if key not in live:
                self._recorders.pop(key).stop(required=False)

    def begin_htm_window_recording(self, suite: str) -> None:
        self._record_suite = suite
        self._next_win_n = 1
        self._text_n = 0
        self._suite_started_at = time.time()
        self._saw_native_mux_windows = False
        if self.text_dir:
            slug = re.sub(r"[^a-z0-9]+", "-", self.name.lower()).strip("-")
            self._step_dir = self.text_dir / f"{slug}-{self.mux}-{suite}-steps"
            if self._step_dir.exists():
                shutil.rmtree(self._step_dir)
            self._step_dir.mkdir(parents=True, exist_ok=True)
            text_output = self.text_dir / f"{slug}-{self.mux}-{suite}-text"
            if text_output.exists():
                shutil.rmtree(text_output)
        self.sync_htm_window_recordings()
        self.snapshot_all_text("begin-suite")

    def end_htm_window_recording(self) -> None:
        for rec in self._recorders.values():
            rec.stop(required=False)
        self._recorders.clear()

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

    def detach_client(self) -> None:
        fail(f"{self.name} does not implement control-mode detach")

    def reattach_client(self) -> None:
        fail(f"{self.name} does not implement control-mode reattach")

    def resize_front_native_window(self, width: int, height: int) -> None:
        fail(f"{self.name} does not implement native window resizing")

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
    if session.mux == "tmux":
        if not session.tmux_has_session():
            fail(f"tmux server exited {when}")
        return
    if not pids_named("htmd"):
        fail(f"htmd exited {when}")


def run_gui_layout_io_tests(session: GuiTerminalSession) -> None:
    """Shared split/tab/keystroke/concurrent-I/O checks for every GUI driver."""
    session.start(session.multiplexer_command())
    session.wait_init()
    session.after_attach()
    _wait_for_native_tab(session)
    print(f"OK: attached to {session.name}", flush=True)
    session.begin_htm_window_recording("layout")
    try:
        _run_gui_layout_io_body(session)
    finally:
        session.end_htm_window_recording()


def _run_gui_layout_io_body(session: GuiTerminalSession) -> None:

    splits_before = session.split_watermark()
    session.keystroke('"d"', "command down")
    session.wait_split(splits_before)
    print("OK: Cmd+D sent split-window", flush=True)
    session.after_first_split()
    session.sync_htm_window_recordings()

    marker = "HTM_E2E_PARITY"
    _submit_command(session, marker)
    session.wait_typed(marker)
    print("OK: keys reached pane", flush=True)
    session.after_marker(marker)
    session.checkpoint("layout-after-marker", oracle=False)

    tabs_before = session.window_watermark()
    session.keystroke('"t"', "command down")
    session.wait_new_window(tabs_before)
    print("OK: Cmd+T sent new-window", flush=True)
    session.sync_htm_window_recordings()

    splits_before = session.split_watermark()
    session.keystroke('"d"', "{command down, shift down}")
    session.wait_split(splits_before)
    print("OK: Cmd+Shift+D sent second split-window", flush=True)
    session.checkpoint("layout-after-tabs-splits", oracle=False)

    time.sleep(0.5)
    stamp = "PARITY"

    def echo_on_focused_pane(tag: str) -> None:
        _submit_command(session, f"echo {tag}")
        session.wait_typed(tag, timeout=12)

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
        if os.name == "nt":
            # One echo that includes the _8 suffix wait_visible looks for.
            return f"echo {tag}_8"
        return f"for i in 1 2 3 4 5 6 7 8; do echo {tag}_$i; sleep 0.08; done &"

    loops = [f"GUI0{stamp}", f"GUI1{stamp}"]
    _submit_command(session, burst_cmd(loops[1]))
    time.sleep(0.2)
    session.previous_pane()
    _submit_command(session, burst_cmd(loops[0]))
    session.wait_visible(f"{loops[0]}_8")
    session.wait_visible(f"{loops[1]}_8")
    time.sleep(0.3)
    session.checkpoint("layout-after-concurrent-output", oracle=False)
    print("OK: concurrent pane output", flush=True)

    time.sleep(0.4)
    panes_before_close = session.split_watermark()
    session.keystroke('"w"', "command down")
    if session.require_kill_pane:
        session.wait_kill_pane(panes_before_close)
        print("OK: Cmd+W sent kill-pane/kill-window", flush=True)
    else:
        time.sleep(0.5)
        print("OK: Cmd+W delivered (kill-pane not required)", flush=True)
    session.sync_htm_window_recordings()
    session.checkpoint("layout-after-close", oracle=False)

    _assert_session_alive(session, "during the happy-path layout test")

    splits_before = session.split_watermark()
    tabs_before = session.window_watermark()
    for _ in range(4):
        session.keystroke('"d"', "command down")
        session.keystroke('"t"', "command down")
        session.keystroke('"d"', "{command down, shift down}")
        session.keystroke('"w"', "command down")
        if os.name == "nt":
            time.sleep(0.5)
    time.sleep(1.0)
    _assert_session_alive(session, "during rapid split/tab/close")
    if session.mux == "htm":
        session.wait_log(
            lambda text: text.count("split-window") >= splits_before
            or text.count("new-window") >= tabs_before,
            15,
            "htmd still accepting packets after race burst",
        )
    elif not session.tmux_has_session():
        fail("tmux server died during rapid split/tab/close")
    print(
        f"OK: rapid split/tab/close did not crash {session.name} or {session.mux}",
        flush=True,
    )
    session.after_layout_suite()
    session.sync_htm_window_recordings()


def run_gui_stress(session: GuiTerminalSession) -> None:
    """Shared bulk-I/O stress: two panes printing while keys still flow."""
    session.start(session.multiplexer_command())
    session.wait_init()
    session.after_attach()
    _wait_for_native_tab(session)
    print("OK: attached", flush=True)
    session.begin_htm_window_recording("stress")
    try:
        _run_gui_stress_body(session)
    finally:
        session.end_htm_window_recording()


def _run_gui_stress_body(session: GuiTerminalSession) -> None:

    splits_before = session.split_watermark()
    session.keystroke('"d"', "command down")
    session.wait_split(splits_before)
    tabs_before = session.window_watermark()
    session.keystroke('"t"', "command down")
    session.wait_new_window(tabs_before)
    session.sync_htm_window_recordings()
    splits_before = session.split_watermark()
    session.keystroke('"d"', "{command down, shift down}")
    session.wait_split(splits_before)
    time.sleep(0.5)
    print("OK: tabs and splits created", flush=True)

    stamp = "PARITY"
    mark_a = f"STA{stamp}"
    mark_b = f"STB{stamp}"

    def echo_tag(tag: str) -> str:
        _submit_command(session, f"echo {tag}")
        session.wait_typed(tag, timeout=12)
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
    session.checkpoint("stress-after-markers", oracle=False)

    if os.name == "nt":
        _submit_command(session, "echo STBULK1")
    else:
        _submit_command(
            session,
            "i=0; while [ $i -lt 200 ]; do echo STBULK1; "
            "i=$((i+1)); sleep 0.01; done &",
        )
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
    if os.name == "nt":
        _submit_command(session, "echo STBULK0")
    else:
        _submit_command(
            session,
            "i=0; while [ $i -lt 200 ]; do echo STBULK0; "
            "i=$((i+1)); sleep 0.01; done &",
        )
    time.sleep(0.3)
    for i in range(8):
        _submit_command(session, f"echo STKEY{stamp}_{i}")
        time.sleep(0.08)

    if session.mux == "tmux":
        session.wait_typed("STBULK", timeout=25)
    else:
        session.wait_log(
            lambda text: text.count("control command: send") >= 4
            and log_has_typed(text, "STBULK"),
            25,
            "bulk send-keys while printers run",
        )
    expected = [f"STKEY{stamp}_{i}" for i in range(8)]
    def _all_stress_output() -> bool:
        snapshot = session.mux_snapshot(wait=0.3)
        return all(
            marker in snapshot for marker in expected + ["STBULK0", "STBULK1"]
        )

    wait_until(
        _all_stress_output,
        25,
        description="all stress output markers",
    )
    time.sleep(0.5)
    session.checkpoint("stress-after-bulk-output", oracle=False)
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
    session.shutdown_multiplexer()


def run_stress_suite(session: GuiTerminalSession) -> None:
    run_gui_stress(session)
    session.shutdown_multiplexer()


def _submit_command(session: GuiTerminalSession, command: str) -> None:
    """Type a shell command and Enter without re-focusing between them."""
    submit = getattr(session, "submit_text", None)
    if callable(submit):
        submit(command)
    else:
        session.keystroke(f'"{command}"')
        session.key_code(36)


def _echo_marker(session: GuiTerminalSession, marker: str) -> None:
    _submit_command(session, f"echo {marker}")
    session.wait_visible(marker)


def _type_ascii_command(session: GuiTerminalSession, command: str) -> None:
    if session.name in ("iTerm2", "WezTerm"):
        command = command.replace("\\", "\\\\")
    _submit_command(session, command)


def _emit_unicode_marker(session: GuiTerminalSession) -> None:
    if os.name == "nt":
        _submit_command(session, f"echo {tmux_cc.CORNER_UNICODE}")
        session.wait_visible(tmux_cc.CORNER_UNICODE)
        return
    # Type ASCII-only octal escapes so this is independent of the host input
    # source while still exercising UTF-8, CJK width, and emoji rendering.
    _type_ascii_command(
        session,
        r"printf 'CORNER_UNICODE_\303\251_\344\270\255_\360\237\230\200\n'",
    )
    session.wait_visible(tmux_cc.CORNER_UNICODE)


def _split_horizontal(session: GuiTerminalSession) -> None:
    before = session.mux_pane_count()
    session.keystroke('"d"', "command down")
    session.wait_mux_pane_count(before + 1)
    session.sync_htm_window_recordings()


def _split_vertical(session: GuiTerminalSession) -> None:
    before = session.mux_pane_count()
    session.keystroke('"d"', "{command down, shift down}")
    session.wait_mux_pane_count(before + 1)
    session.sync_htm_window_recordings()


def _new_window(session: GuiTerminalSession) -> None:
    before = session.mux_window_count()
    session.keystroke('"t"', "command down")
    session.wait_mux_window_count(before + 1)
    session.sync_htm_window_recordings()


def _window_id_with_text(session: GuiTerminalSession, marker: str) -> str:
    for pane in htm_gui_parity.parse_panes(session.mux_snapshot(wait=0.4)):
        if marker in (pane.get("body") or ""):
            wid = pane.get("wid") or ""
            return wid if str(wid).startswith("@") else f"@{wid}"
    return ""


def _active_shell_pid(session: GuiTerminalSession) -> int:
    if session.mux == "tmux":
        raw = session.tmux_cmd("display-message", "-p", "#{pane_pid}").strip()
        if raw.isdigit():
            return int(raw)
    for pane in htm_gui_parity.parse_panes(session.mux_snapshot(wait=0.4)):
        if pane.get("current") != "1":
            continue
        raw = pane.get("shell_pid") or ""
        if str(raw).isdigit():
            return int(raw)
    return 0


def _kill_focused(
    session: GuiTerminalSession,
    panes: Optional[int] = None,
    windows: Optional[int] = None,
    *,
    while_writing: bool = False,
    writer_marker: Optional[str] = None,
) -> None:
    """Close the active pane the way iTerm2+tmux -CC does.

    Idle panes: type ``exit`` so the PTY dies (same as a user leaving the
    shell). A pane that is still running a command ignores ``exit``, so
    those use Cmd+W (kill-pane) with Accessibility snapshots suppressed so
    focus stays on the native pane.
    """
    before_p = session.mux_pane_count()
    before_w = session.mux_window_count()
    tmux_window = ""
    writer_window = ""
    if while_writing:
        writer_window = _window_id_with_text(
            session, writer_marker or "WRTICKW"
        )
    if session.mux == "tmux":
        tmux_window = writer_window or session.tmux_cmd(
            "display-message", "-p", "#{window_id}"
        ).strip()
    try:
        session.focus_native_window()
    except Exception:
        pass
    session._capturing_text = True
    try:
        if while_writing:
            session.keystroke('"w"', "command down")
        else:
            _submit_command(session, "exit")
    finally:
        session._capturing_text = False

    def _closed() -> bool:
        return (
            session.mux_pane_count() < before_p
            or session.mux_window_count() < before_w
        )

    deadline = time.time() + 2.0
    while time.time() < deadline and not _closed():
        time.sleep(0.1)
    if while_writing and not _closed():
        if session.mux == "tmux":
            args = ["kill-window"]
            if tmux_window:
                args.extend(["-t", tmux_window])
            session.tmux_cmd(*args)
        else:
            pid = 0
            marker = writer_marker or "WRTICKW"
            for pane in htm_gui_parity.parse_panes(session.mux_snapshot(wait=0.4)):
                if marker in (pane.get("body") or ""):
                    raw = pane.get("shell_pid") or ""
                    if str(raw).isdigit():
                        pid = int(raw)
                        break
            if pid <= 1:
                pid = _active_shell_pid(session)
            if pid > 1:
                try:
                    os.killpg(pid, signal.SIGKILL)
                except OSError:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except OSError:
                        pass
    wait_until(_closed, 20, description="pane or window closed")
    if panes is not None:
        session.wait_mux_pane_count(panes)
    if windows is not None:
        session.wait_mux_window_count(windows)
    session.sync_htm_window_recordings()
    # After a pane exits, refocus the surviving native HTM window so the next
    # split/new-window action does not land on a dead or gateway window.
    try:
        session.focus_native_window()
    except Exception:
        pass


def _start_writer(session: GuiTerminalSession, tag: str) -> None:
    if os.name == "nt":
        # Bare `for /L` with no delay floods PaneScreen history (2000 lines)
        # and scrolls early markers (e.g. CORNER_ROOT) out before detach
        # checkpoints that still require them. ping ~1s keeps the writer
        # slow enough that history still holds the root marker.
        _submit_command(
            session,
            f"for /L %i in (1,0,1) do @echo {tag}& "
            f"ping -n 1 127.0.0.1 >nul",
        )
    else:
        _submit_command(session, f"while :; do echo {tag}; sleep 0.05; done")
    wait_until(
        lambda: session.mux_snapshot(wait=0.2).count(tag) >= 2,
        20,
        description=f"writer emitted {tag}",
    )


def run_gui_corners(session: GuiTerminalSession) -> None:
    """Detach, kill/recreate, kill-while-writing, titles vs iTerm2+tmux -CC."""
    session.start(session.multiplexer_command())
    session.wait_init()
    session.after_attach()
    _wait_for_native_tab(session)
    print(f"OK: attached to {session.name} mux={session.mux}", flush=True)
    session.begin_htm_window_recording("corners")
    try:
        _run_gui_corners_body(session)
    finally:
        session.end_htm_window_recording()


def _run_gui_corners_body(session: GuiTerminalSession) -> None:
    session.wait_mux_pane_count(1, timeout=10)
    session.checkpoint("after-attach")

    _echo_marker(session, tmux_cc.CORNER_ROOT)
    session.checkpoint("after-root")
    _emit_unicode_marker(session)
    session.checkpoint("after-unicode")
    if os.name == "nt":
        _type_ascii_command(
            session,
            "for /L %i in (1,1,40) do @echo SCROLLBACK_%i",
        )
    else:
        _type_ascii_command(
            session,
            "i=1; while [ $i -le 40 ]; do echo SCROLLBACK_$i; "
            "i=$((i+1)); done",
        )
    session.wait_visible(tmux_cc.CORNER_SCROLL_LAST)
    session.checkpoint("after-scrollback")
    if os.name == "nt":
        _echo_marker(session, tmux_cc.CORNER_AFTER_ALT)
    else:
        _type_ascii_command(
            session,
            r"printf '\033[?1049h\101\114\124\137\123\103\122\105\105\116"
            r"\033[?1049l'; echo AFTER_ALT",
        )
    session.wait_visible(tmux_cc.CORNER_AFTER_ALT)
    session.checkpoint("after-alternate-screen")
    if session.supports_native_resize:
        session.resize_front_native_window(900, 700)

        def _resized() -> bool:
            panes = htm_gui_parity.parse_panes(session.mux_snapshot(wait=0.3))
            return bool(
                panes
                and max(int(pane["cols"]) for pane in panes) >= 100
                and max(int(pane["rows"]) for pane in panes) >= 30
            )

        wait_until(_resized, 20, description="native window resize reached mux")
        session.checkpoint("after-native-resize")
        print("OK: native resize updated pane geometry", flush=True)

    if session.supports_detach:
        _start_writer(session, tmux_cc.WRTICKR)
        session.checkpoint("before-writer-detach")
        session.detach_client()
        session.reattach_client()
        session.wait_visible(tmux_cc.WRTICKR)
        session.checkpoint("after-writer-detach-reattach")
        session.focus_native_window()
        if session.mux == "tmux":
            session.tmux_cmd("send-keys", "C-c")
        else:
            session.keystroke('"c"', "control down")
        time.sleep(0.4)
        session.focus_native_window()
        _echo_marker(session, tmux_cc.AFTER_REATTACH)
        session.checkpoint("after-detach-reattach")
        print("OK: detach/reattach kept pane contents and active output", flush=True)
    else:
        print(f"SKIP detach/reattach on {session.name}", flush=True)

    _split_horizontal(session)
    session.checkpoint("after-split")
    _echo_marker(session, tmux_cc.CORNER_SPLIT)
    session.checkpoint("after-split-echo")

    _kill_focused(session, panes=1, windows=1)
    session.wait_visible(tmux_cc.CORNER_ROOT)
    session.checkpoint("after-kill-pane")
    print("OK: killed pane then session still had the surviving marker", flush=True)

    _split_horizontal(session)
    _echo_marker(session, tmux_cc.CORNER_SPLIT2)
    session.checkpoint("after-split-again")

    _new_window(session)
    _echo_marker(session, tmux_cc.CORNER_WIN2)
    session.checkpoint("after-new-window")

    _new_window(session)
    _echo_marker(session, tmux_cc.CORNER_WIN3)
    session.checkpoint("after-third-window")

    _kill_focused(session, panes=3, windows=2)
    session.wait_visible(tmux_cc.CORNER_WIN2)
    session.checkpoint("after-kill-window")
    print("OK: killed window then created replacements stay healthy", flush=True)

    _new_window(session)
    _echo_marker(session, tmux_cc.CORNER_WIN4)
    session.checkpoint("after-replace-window")

    if session.mux == "tmux":
        session.tmux_cmd("send-keys", "sleep 25", "Enter")
    elif os.name == "nt":
        _submit_command(session, "timeout /t 25")
    else:
        _submit_command(session, "sleep 25")
    title = tmux_cc.TITLE_SLEEP_WIN if os.name == "nt" else tmux_cc.TITLE_SLEEP
    session.wait_window_named(title)
    session.checkpoint("after-title-sleep")
    print(f"OK: automatic-rename window title is {title}", flush=True)
    if session.mux == "tmux":
        session.tmux_cmd("send-keys", "C-c")
    else:
        session.keystroke('"c"', "control down")
    time.sleep(0.4)
    _assert_session_alive(session, "after interrupting sleep")

    _split_horizontal(session)
    _start_writer(session, tmux_cc.WRTICKP)
    session.checkpoint("after-writer-pane")
    _kill_focused(session, panes=4, windows=3, while_writing=True)
    _echo_marker(session, tmux_cc.AFTER_KILL_PANE_WRITER)
    session.checkpoint("after-kill-writer-pane")
    print("OK: killed pane while a command was writing", flush=True)

    _new_window(session)
    _start_writer(session, tmux_cc.WRTICKW)
    session.checkpoint("after-writer-window")
    _kill_focused(session, while_writing=True, writer_marker=tmux_cc.WRTICKW)
    session.checkpoint("after-kill-writer-window")
    print("OK: killed window while a command was writing", flush=True)


def run_corners_suite(session: GuiTerminalSession) -> None:
    run_gui_corners(session)
    _assert_session_alive(session, "after corners test")
    session.shutdown_multiplexer()


SUITES: dict[str, Callable[[GuiTerminalSession], None]] = {
    "layout": run_layout_suite,
    "stress": run_stress_suite,
    "corners": run_corners_suite,
}
SUITE_ORDER = ("layout", "stress", "corners")

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
        help="layout, stress, corners, comma-separated names, or all "
        f"(default: {default_suite})",
    )
    parser.add_argument(
        "--record-video",
        nargs="?",
        const="/tmp/htm-e2e-videos",
        default=None,
        help="record each HTM-launched window (macOS .mov, Windows .mp4) "
        "(default directory: /tmp/htm-e2e-videos)",
    )
    parser.add_argument(
        "--mux",
        default="htm",
        help="htm, tmux, or both (tmux -CC is ground truth; default: htm)",
    )


def run_emulator_main(module: object, default_suite: str = "layout") -> int:
    """CLI entry used by per-emulator scripts and the unified runner."""
    name = getattr(module, "NAME", "GUI")
    platforms = getattr(module, "PLATFORMS", ("darwin",))
    if sys.platform not in platforms:
        skip(f"{name} HTM e2e requires {' or '.join(platforms)}")

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
    suites = parse_suites(args.suite)
    muxes = muxes_for_suites(parse_mux(getattr(args, "mux", "htm")), suites)
    tmux_bin = find_tmux_bin() if "tmux" in muxes else None
    video_dir = Path(args.record_video) if getattr(args, "record_video", None) else None
    text_dir = video_dir or (
        Path(tempfile.gettempdir()) / "htm-e2e-videos"
        if os.name == "nt"
        else Path("/tmp/htm-e2e-videos")
    )
    text_dir.mkdir(parents=True, exist_ok=True)
    if video_dir:
        print(f"Recording windows to {video_dir}", flush=True)

    last_name = name
    for mux in muxes:
        for suite in suites:
            session: GuiTerminalSession = open_session(htm, htmd, args)
            session.mux = mux
            session.tmux_bin = tmux_bin
            session.tmux_socket = f"et-e2e-{os.getpid()}-{mux}-{suite}"
            if video_dir:
                session.video_dir = video_dir
            session.text_dir = text_dir
            last_name = session.name
            print(
                f"Using {session.name}={getattr(session, 'app', session.name)}",
                flush=True,
            )
            print(f"Using mux={mux}", flush=True)
            if mux == "htm":
                print(f"Using htm={htm}", flush=True)
                print(f"Using htmd={htmd}", flush=True)
            else:
                print(
                    f"Using tmux={tmux_bin} socket={session.tmux_socket}",
                    flush=True,
                )
            try:
                run_gui_suites(session, [suite])
            finally:
                session.end_htm_window_recording()
                session.stop()
                if mux == "htm":
                    kill_named("htmd")
                else:
                    session.shutdown_multiplexer()
                session.warn_leftovers()
    verify_gui_parity_against_tmux_cc(last_name, text_dir, muxes, suites)
    print(f"PASS: {last_name} e2e mux={','.join(muxes)} ({', '.join(suites)})", flush=True)
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
        skip(f"{args.emulator} HTM e2e requires {' or '.join(platforms)}")

    htm = find_htm_bin(args.htm)
    htmd = find_htmd_bin(args.htmd, htm)
    suites = parse_suites(args.suite)
    muxes = muxes_for_suites(parse_mux(getattr(args, "mux", "htm")), suites)
    tmux_bin = find_tmux_bin() if "tmux" in muxes else None
    video_dir = Path(args.record_video) if getattr(args, "record_video", None) else None
    text_dir = video_dir or (
        Path(tempfile.gettempdir()) / "htm-e2e-videos"
        if os.name == "nt"
        else Path("/tmp/htm-e2e-videos")
    )
    text_dir.mkdir(parents=True, exist_ok=True)
    if video_dir:
        print(f"Recording windows to {video_dir}", flush=True)

    last_name = args.emulator
    for mux in muxes:
        for suite in suites:
            session: GuiTerminalSession = module.open_session(htm, htmd, args)
            session.mux = mux
            session.tmux_bin = tmux_bin
            session.tmux_socket = f"et-e2e-{os.getpid()}-{mux}-{suite}"
            if video_dir:
                session.video_dir = video_dir
            session.text_dir = text_dir
            last_name = session.name
            print(
                f"Using {session.name}={getattr(session, 'app', session.name)}",
                flush=True,
            )
            print(f"Using mux={mux}", flush=True)
            if mux == "htm":
                print(f"Using htm={htm}", flush=True)
                print(f"Using htmd={htmd}", flush=True)
            else:
                print(
                    f"Using tmux={tmux_bin} socket={session.tmux_socket}",
                    flush=True,
                )
            try:
                run_gui_suites(session, [suite])
            finally:
                session.end_htm_window_recording()
                session.stop()
                if mux == "htm":
                    kill_named("htmd")
                else:
                    session.shutdown_multiplexer()
                session.warn_leftovers()
    verify_gui_parity_against_tmux_cc(last_name, text_dir, muxes, suites)
    print(f"PASS: {last_name} e2e mux={','.join(muxes)} ({', '.join(suites)})", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
