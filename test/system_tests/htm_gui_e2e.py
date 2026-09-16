#!/usr/bin/env python3
"""Shared GUI HTM e2e framework.

Tests (layout, stress, corners, affinities, control-plane) are emulator-agnostic.
Each terminal plugs in by implementing ``GuiTerminalSession`` and exporting
``NAME``, ``add_arguments``, ``apply_args``, and ``open_session``. Run one
emulator:

  python3 test/system_tests/htm_gui_e2e.py --emulator ghostty --suite all
  python3 test/system_tests/hyper_htm_e2e.py --suite stress

Protocol checks read htmd ``control command:`` logs. The control-plane suite
asserts iTerm2-compatible gateway UX (command menu, Esc detach, X force-quit,
L logging, C run command) plus native mux windows for panes.
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

# Exact iTerm2 / WezTerm tmux -CC gateway menu (CRLF normalized to LF for compare).
EXPECTED_TMUX_COMMAND_MENU = (
    "** tmux mode started **\n\n"
    "Command Menu\n"
    "----------------------------\n"
    "esc    Detach cleanly.\n"
    "  X    Force-quit tmux mode.\n"
    "  L    Toggle logging.\n"
    "  C    Run tmux command."
)

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


def _recording_window_key(win: dict) -> str:
    """Stable identity for screencapture — ignore title churn.

    iTerm2 rewrites the AX window name on automatic-rename and when it
    embeds cols×rows (``— 127✕39``). Keying recordings on that name
    produced one short ``winNN`` file per rename while Ghostty (stable
    cwd title) kept a single continuous capture. Use the AX id when
    present, otherwise the top-left position so resize alone does not
    split the file either.
    """
    if win.get("id"):
        return f"id:{win['id']}"
    return f"pos:{int(win['x'])},{int(win['y'])}"


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
    # iTerm2 Move Session → move-pane / break-pane (layout epilogue).
    supports_move_session = False

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
        affinities = self.tmux_affinities_json()
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
        chunks: list[str] = [f"# affinities: {affinities}\n"]
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

    def tmux_affinities_json(self) -> str:
        """Read ``@affinities`` and render the list-of-lists JSON for dumps."""
        try:
            raw = self.tmux_cmd("show", "-v", "-q", "@affinities")
        except Exception:
            raw = ""
        groups = htm_gui_parity.parse_affinity_groups(raw)
        return htm_gui_parity.affinities_json(groups)

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
        """Record this mux's dump and assert contents / affinities.

        Affinities are always checked (corners vs recorded iTerm2 JSON;
        layout/stress vs Cmd+T one-OS-window semantics). When ``oracle`` is
        true, pane text and topology are also checked against the corners
        table.
        """
        dump = self.mux_snapshot()
        body = f"# action: {step_id}\n# mux={self.mux}\n\n{dump}"
        if self._step_dir:
            self._step_dir.mkdir(parents=True, exist_ok=True)
            (self._step_dir / f"{step_id}.txt").write_text(body, encoding="utf-8")
        sp = htm_gui_parity.spurious_prompt_sp_lines(dump)
        if sp:
            fail(
                f"{self.mux} checkpoint {step_id} has spurious zsh PROMPT_SP '%': "
                + "; ".join(sp)
                + f"\n{dump[:2000]}"
            )
        if oracle:
            errors = tmux_cc.check_step(step_id, dump)
        else:
            errors = tmux_cc.check_affinities(step_id, dump)
        if errors:
            fail(
                f"{self.mux} checkpoint {step_id} diverged from "
                "iTerm2+tmux -CC: "
                + "; ".join(errors)
                + f"\n{dump[:2000]}"
            )
        if oracle:
            print(
                f"OK: {step_id} matches iTerm2+tmux -CC oracle ({self.mux})",
                flush=True,
            )
        else:
            print(
                f"OK: {step_id} affinities + parity dump recorded ({self.mux})",
                flush=True,
            )
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
        down = cg.CGEventCreateMouseEvent(
            None, kCGEventLeftMouseDown, point, kCGMouseButtonLeft
        )
        if not down:
            fail(f"CGEventCreateMouseEvent failed at {int(x)},{int(y)}")
        cg.CGEventPost(kCGHIDEventTap, down)
        cg.CFRelease(down)
        time.sleep(0.05)
        up = cg.CGEventCreateMouseEvent(
            None, kCGEventLeftMouseUp, point, kCGMouseButtonLeft
        )
        cg.CGEventPost(kCGHIDEventTap, up)
        cg.CFRelease(up)
        time.sleep(0.2)
        self.snapshot_all_text(f"click-{int(x)}-{int(y)}")

    def focus_native_window(self) -> None:
        """Raise a tmux -CC native pane window, not the gateway PTY."""
        launched = self.launched_windows()
        self.focus()
        if not launched:
            return
        win = _newest_native(launched)
        self._front_native = win
        self._raise_ax_window(win)

    def native_recording_key(self, win: Optional[dict] = None) -> str:
        """Stable AX identity for a mux OS window (ignores title churn)."""
        target = win
        if target is None:
            launched = self.launched_windows()
            if not launched:
                return ""
            target = getattr(self, "_front_native", None) or _newest_native(launched)
        return _recording_window_key(target)

    def focus_native_by_key(self, key: str, timeout: float = 15.0) -> dict:
        """Raise the mux OS window with the given recording key (not newest)."""

        def _find() -> Optional[dict]:
            for win in self.launched_windows():
                if _recording_window_key(win) == key:
                    return win
            return None

        wait_until(
            lambda: _find() is not None,
            timeout,
            description=f"native mux window key={key}",
        )
        win = _find()
        assert win is not None
        self._front_native = win
        self._raise_ax_window(win)
        return win

    def wait_native_mux_windows(self, count: int, timeout: float = 20.0) -> None:
        wait_until(
            lambda: len(self.launched_windows()) >= count,
            timeout,
            description=f"{count} native mux OS windows",
        )

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
        # Prefer AX id: raising reshuffles System Events window indices.
        wid = int(win.get("id") or 0)
        if wid:
            finder = f"(first window whose id is {wid})"
            try:
                run_osascript(
                    f'''
tell application "System Events"
  tell {self.ax_tell_target()}
    set frontmost to true
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
            except (subprocess.CalledProcessError, SystemExit):
                wid = 0
        if not wid:
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
        # Refresh frame after raise — stacked windows share origins until
        # raised; click the live frontmost matching window when possible.
        live = next(
            (
                w
                for w in self.ax_windows()
                if (wid and w.get("id") == wid)
                or (
                    not wid
                    and abs(float(w["x"]) - float(win["x"])) <= 40
                    and abs(float(w["y"]) - float(win["y"])) <= 40
                    and abs(float(w["w"]) - float(win["w"])) <= 48
                    and abs(float(w["h"]) - float(win["h"])) <= 48
                )
            ),
            win,
        )
        time.sleep(0.2)
        # Prefer an Accessibility click on the raised window so AppKit
        # marks it key / first-responder (CGEvent alone often does not
        # when another window of the same app was already frontmost).
        try:
            run_osascript(
                f'''
tell application "System Events"
  tell {self.ax_tell_target()}
    set frontmost to true
    set w to window {int(live["index"])}
    try
      set value of attribute "AXMain" of w to true
    end try
    try
      set value of attribute "AXFocused" of w to true
    end try
    click at {{{int(float(live["x"]) + float(live["w"]) * 0.72)}, {int(float(live["y"]) + float(live["h"]) * 0.55)}}}
  end tell
end tell
'''
            )
            time.sleep(0.25)
        except (subprocess.CalledProcessError, SystemExit):
            self.click_screen(
                float(live["x"]) + float(live["w"]) * 0.72,
                float(live["y"]) + float(live["h"]) * 0.55,
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
            key = _recording_window_key(win)
            live.add(key)
            region = (int(win["x"]), int(win["y"]), int(win["w"]), int(win["h"]))
            existing = self._recorders.get(key)
            if existing is not None:
                # Title renames must not split the file; a real move/resize
                # needs a new capture region (screencapture -R is fixed).
                ox, oy, ow, oh = existing.region
                x, y, w, h = region
                if (
                    abs(ox - x) <= 24
                    and abs(oy - y) <= 24
                    and abs(ow - w) <= 48
                    and abs(oh - h) <= 48
                ):
                    continue
                existing.stop(required=False)
                del self._recorders[key]
            n = self._next_win_n
            self._next_win_n += 1
            slug = re.sub(r"[^a-z0-9]+", "-", self.name.lower()).strip("-")
            ext = ".mp4" if os.name == "nt" else ".mov"
            path = (
                self.video_dir
                / f"{slug}-{self.mux}-{self._record_suite}-win{n:02d}{ext}"
            )
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
        self.after_split(vertical=False)

    def after_split(self, *, vertical: bool = False) -> None:
        """Optional: focus the new pane after a horizontal or vertical split."""

    def after_new_window(self) -> None:
        """Optional: focus the new tmux window/tab after Cmd+T."""

    def after_marker(self, marker: str) -> None:
        """Optional: assert visible contents after typing ``marker``."""

    def rendered_terminal_text(self) -> Optional[str]:
        """Rendered text of the currently focused emulator pane, if available."""
        return None

    def run_move_session_checks(self) -> None:
        """Optional: Move Session → move-pane / break-pane (iTerm2)."""

    def after_layout_suite(self) -> None:
        """Shared layout epilogue: detach/reattach when the driver supports it."""
        if not self.supports_detach:
            return
        self.detach_client()
        self.reattach_client()

    def gateway_text(self) -> str:
        """Visible text of the tmux -CC / htm control-plane (gateway) surface."""
        fail(f"{self.name} does not implement gateway_text()")

    def detach_client(self) -> None:
        """Esc on the gateway: detach cleanly; mux server must survive."""
        log_path = self.log_file if self.mux == "htm" else None
        log_before = (
            log_path.read_text(errors="replace")
            if log_path is not None and log_path.is_file()
            else ""
        )
        watermark = len(log_before)

        self.focus_gateway()
        self.key_code(53)  # Escape
        if self.mux == "tmux":
            wait_until(
                lambda: self.tmux_has_session() and self.tmux_client_count() == 0,
                15,
                description=f"{self.name} Esc detached tmux client",
            )
        else:

            def _detached() -> bool:
                if log_path is None or not log_path.is_file():
                    return False
                new = log_path.read_text(errors="replace")[watermark:]
                return any(
                    command.strip() in ("detach", "detach-client")
                    for command in control_commands(new)
                )

            wait_until(_detached, 15, description=f"{self.name} Esc detached htm")
            wait_until(
                lambda: bool(pids_named("htmd")) and ipc_path().exists(),
                15,
                description="htmd survived Esc detach",
            )
            self._reattach_log_path = log_path
            self._reattach_log_watermark = (
                len(log_path.read_text(errors="replace"))
                if log_path is not None and log_path.is_file()
                else 0
            )
        print(f"OK: {self.name} detached without killing {self.mux}", flush=True)

    def reattach_client(self) -> None:
        """Re-run ``tmux -CC attach`` / ``htm`` in the gateway window."""
        self.focus_gateway()
        attach = (
            f"{self.tmux_bin} -L {self.tmux_socket} -f /dev/null "
            "-CC attach-session"
            if self.mux == "tmux"
            else str(self.htm)  # no -x: must not kill the surviving htmd
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
                description=f"{self.name} tmux -CC client reattached",
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

            wait_until(
                _reattached, 20, description=f"{self.name} htm control client reattached"
            )
        self.after_attach()
        self.sync_htm_window_recordings()
        print(f"OK: {self.name} reattached to {self.mux}", flush=True)

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
    """Wait until a mux surface exists outside the control-plane window."""
    deadline = time.time() + 8
    while time.time() < deadline:
        if session.window_count() >= 2:
            break
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
    _assert_no_spurious_percent(session, "immediate layout Cmd+D horizontal split")
    session.after_first_split()
    _assert_no_spurious_percent(session, "layout Cmd+D horizontal split")
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
    _assert_no_spurious_percent(
        session, "immediate layout Cmd+Shift+D vertical split"
    )
    session.after_split(vertical=True)
    _assert_no_spurious_percent(session, "layout Cmd+Shift+D vertical split")
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
    if session.supports_move_session:
        session.run_move_session_checks()
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
    _assert_no_spurious_percent(session, "immediate stress Cmd+D horizontal split")
    session.after_split(vertical=False)
    _assert_no_spurious_percent(session, "stress Cmd+D horizontal split")
    tabs_before = session.window_watermark()
    session.keystroke('"t"', "command down")
    session.wait_new_window(tabs_before)
    session.sync_htm_window_recordings()
    session.after_new_window()
    splits_before = session.split_watermark()
    session.keystroke('"d"', "{command down, shift down}")
    session.wait_split(splits_before)
    _assert_no_spurious_percent(
        session, "immediate stress Cmd+Shift+D vertical split"
    )
    session.after_split(vertical=True)
    _assert_no_spurious_percent(session, "stress Cmd+Shift+D vertical split")
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

    # Re-assert mux key focus after checkpoint I/O / AX churn (Hyper).
    raise_input = getattr(session, "_raise_mux_for_input", None)
    if callable(raise_input):
        raise_input(click=True)
        time.sleep(0.3)

    if os.name == "nt":
        _submit_command(session, "echo STBULK1")
    else:
        _submit_command(
            session,
            "for i in 1 2 3 4 5 6 7 8 9 10; do echo STBULK1; sleep 0.05; done &",
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
            "for i in 1 2 3 4 5 6 7 8 9 10; do echo STBULK0; sleep 0.05; done &",
        )
    time.sleep(0.3)
    for i in range(8):
        _submit_command(session, f"echo STKEY{stamp}_{i}")
        time.sleep(0.08)

    if session.mux == "tmux":
        session.wait_visible("STBULK", timeout=25)
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
    # Require the marker on its own line (command output). Matching the typed
    # ``echo MARKER`` substring alone hides missing Enter keystrokes.
    wait_until(
        lambda: any(
            line.strip() == marker
            for line in session.mux_snapshot(wait=0.35).splitlines()
        ),
        20.0,
        description=f"executed marker {marker}",
    )


def _assert_no_spurious_percent(
    session: GuiTerminalSession, action: str
) -> None:
    """Check both mux state and the emulator's rendered active-pane buffer."""
    dump = session.mux_snapshot(wait=0.4)
    backend_hits = htm_gui_parity.spurious_prompt_sp_lines(dump)
    rendered = session.rendered_terminal_text()
    rendered_hits = (
        htm_gui_parity.spurious_prompt_sp_lines(rendered)
        if rendered is not None
        else []
    )
    if backend_hits or rendered_hits:
        detail = []
        if backend_hits:
            detail.append("mux=" + ", ".join(backend_hits))
        if rendered_hits:
            detail.append("rendered=" + ", ".join(rendered_hits))
        fail(
            f"{session.name} {session.mux} {action} has spurious "
            "zsh PROMPT_SP '%': "
            + "; ".join(detail)
            + f"\nMUX:\n{dump[:2000]}"
            + (f"\nRENDERED:\n{rendered[:2000]}" if rendered is not None else "")
        )
    checked = "mux + rendered terminal" if rendered is not None else "mux"
    print(f"OK: no spurious % after {action} ({checked})", flush=True)


def _type_ascii_command(session: GuiTerminalSession, command: str) -> None:
    if session.name in ("iTerm2", "WezTerm", "Hyper", "Ghostty"):
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
    deadline = time.time() + 8.0
    while time.time() < deadline:
        if session.mux_pane_count() >= before + 1:
            break
        time.sleep(0.2)
    else:
        if session.mux == "tmux":
            # Hyper may still focus the gateway after mux side-channel
            # ops; split tmux's current pane directly.
            session.tmux_cmd("split-window", "-h")
        else:
            fail(f"timed out waiting for {before + 1} live panes")
    session.wait_mux_pane_count(before + 1)
    _assert_no_spurious_percent(session, "immediate horizontal split")
    session.sync_htm_window_recordings()
    session.after_split(vertical=False)
    _assert_no_spurious_percent(session, "horizontal split")


def _split_vertical(session: GuiTerminalSession) -> None:
    before = session.mux_pane_count()
    session.keystroke('"d"', "{command down, shift down}")
    session.wait_mux_pane_count(before + 1)
    _assert_no_spurious_percent(session, "immediate vertical split")
    session.sync_htm_window_recordings()
    session.after_split(vertical=True)
    _assert_no_spurious_percent(session, "vertical split")


def _new_window(session: GuiTerminalSession) -> None:
    """Cmd+T: new tmux window as a tab in the focused OS window's affinity."""
    before = session.mux_window_count()
    # Hyper: click the intended OS window before Cmd+T. Modifier keystrokes
    # skip the mouse click, and Electron often leaves the newest OS window
    # key — so the follower would join the wrong affinity group.
    if session.name in ("Hyper", "Windows Terminal"):
        raise_input = getattr(session, "_raise_mux_for_input", None)
        if callable(raise_input):
            try:
                raise_input(click=True)
            except Exception:
                pass
    session.keystroke('"t"', "command down")
    session.wait_mux_window_count(before + 1)
    session.sync_htm_window_recordings()
    session.after_new_window()


def _live_affinity_groups(dump: str) -> list[list[int]]:
    """Affinity groups with live window ids (not ranked — for focus/routing)."""
    groups = htm_gui_parity.parse_affinities_from_dump(dump)
    if groups is None:
        return []
    live = {int(pane["wid"]) for pane in htm_gui_parity.parse_panes(dump)}
    return [
        [wid for wid in group if wid in live]
        for group in groups
        if any(wid in live for wid in group)
    ]


def _select_mux_window(session: GuiTerminalSession, wid: int) -> None:
    """Make ``@wid`` tmux/htm's current window (side channel, not GUI focus)."""
    target = f"@{wid}" if not str(wid).startswith("@") else str(wid)
    if session.mux == "tmux":
        session.tmux_cmd("select-window", "-t", target)
        time.sleep(0.35)
        return
    # HTM: reuse the tmux-compatible control command via a one-shot client
    # when available; otherwise best-effort through display-message is N/A.
    opener = getattr(session, "htm_select_window", None)
    if callable(opener):
        opener(int(str(target).lstrip("@")))
        time.sleep(0.35)


def _focus_os_window_showing(session: GuiTerminalSession, marker: str) -> dict:
    """Raise the native mux OS window whose affinity group contains ``marker``.

    The marker may live in a non-frontmost tab of that OS window; matching only
    the active pane would miss older tabs (e.g. AFF_A0 after Cmd+T created
    AFF_A1 in the same affinity). Uses absolute window ids (not ranked;
    ranking is only for oracle compare).

    Also issues a mux ``select-window`` so Ghostty's Cmd+T affinity (driven by
    ``%session-window-changed`` / active_window) joins the right group even when
    Accessibility cannot make the other OS window key.
    """
    target_wid = _window_id_with_text(session, marker)
    if not target_wid:
        fail(f"no tmux window shows {marker!r}")
    target = int(target_wid.lstrip("@"))
    launched = session.launched_windows()
    if not launched:
        fail(f"no native mux windows while looking for {marker}")

    _select_mux_window(session, target)
    time.sleep(0.5)

    # Hyper: after select-window, mux "current" stays in the target group no
    # matter which OS window we raise — so the AX loop below would happily
    # bind ``_front_native`` to the newest window B. Prefer the OS window
    # recorded when this marker (or its affinity sibling) was created.
    if session.name in ("Hyper", "Windows Terminal"):
        hosts = getattr(session, "_marker_host_window", {}) or {}
        recorded = hosts.get(marker)
        if recorded is None:
            dump = session.mux_snapshot(wait=0.2)
            for group in _live_affinity_groups(dump):
                if target not in group:
                    continue
                for sibling in group:
                    for m, win in hosts.items():
                        wid = _window_id_with_text(session, m)
                        if wid and int(wid.lstrip("@")) == sibling:
                            recorded = win
                            break
                    if recorded is not None:
                        break
                break
        if recorded is not None:
            live = None
            tid = int(recorded.get("id") or 0)
            tkey = _recording_window_key(recorded)
            for w in session.launched_windows():
                if tid and int(w.get("id") or 0) == tid:
                    live = w
                    break
                if _recording_window_key(w) == tkey:
                    live = w
                    break
            if live is not None:
                session._raise_ax_window(live)
                session._front_native = live
                raise_input = getattr(session, "_raise_mux_for_input", None)
                if callable(raise_input):
                    try:
                        raise_input(click=True)
                    except Exception:
                        pass
                time.sleep(0.35)
                return live

    def _current_in_target_group() -> Optional[dict]:
        dump = session.mux_snapshot(wait=0.2)
        groups = _live_affinity_groups(dump)
        current = None
        for pane in htm_gui_parity.parse_panes(dump):
            if pane.get("current") == "1":
                current = int(pane["wid"])
                break
        if current is None:
            return None
        for group in groups:
            if current in group and target in group:
                front = session.launched_windows()
                return front[0] if front else launched[0]
        return None

    # Ghostty: AX raise / click often fails to transfer AppKit key focus.
    # select-window (tmux side channel, or HTM via gateway C prompt) is
    # enough for Cmd+T via %session-window-changed → active_window.
    if session.name == "Ghostty":
        hit = _current_in_target_group()
        if hit is not None:
            time.sleep(0.5)
            session._front_native = hit
            return hit
        fail(f"no OS window affinity contains {marker!r} (@{target})")

    # Best-effort AX raise of each mux window (works for iTerm2 / Hyper).
    keys = [_recording_window_key(w) for w in launched]
    for key in keys:
        live = session.launched_windows()
        win = next(
            (w for w in live if _recording_window_key(w) == key),
            None,
        )
        if win is None:
            for w in live:
                if key.startswith("pos:") and _recording_window_key(w).startswith(
                    "pos:"
                ):
                    try:
                        _, rest = key.split(":", 1)
                        ox, oy = (int(x) for x in rest.split(",", 1))
                        if abs(int(w["x"]) - ox) <= 40 and abs(int(w["y"]) - oy) <= 40:
                            win = w
                            break
                    except ValueError:
                        pass
        if win is None:
            continue
        session._raise_ax_window(win)
        deadline = time.time() + 1.2
        while time.time() < deadline:
            if _current_in_target_group() is not None:
                # Keep the window we raised — not launched_windows()[0], which
                # may still be the newest OS window B. Hyper Cmd+T follows
                # ``_front_native`` / key focus into that host's affinity.
                session._front_native = win
                return win
            time.sleep(0.15)

    hit = _current_in_target_group()
    if hit is not None:
        session._front_native = hit
        return hit

    fail(f"no OS window affinity contains {marker!r} (@{target})")


def _new_os_window(session: GuiTerminalSession) -> None:
    """Open a tmux window in a new OS window (empty affinity).

    Stock iTerm2: Cmd+N is *not* tmux-aware (``newWindow:possiblyTmux:NO``);
    the control-mode path is Shell → tmux → New Tmux Window. Emulators that
    map Cmd+N to the same action can override ``new_tmux_os_window``.
    """
    before_w = session.mux_window_count()
    before_os = len(session.launched_windows())
    opener = getattr(session, "new_tmux_os_window", None)
    if callable(opener):
        opener()
    else:
        session.keystroke('"n"', "command down")
    session.wait_mux_window_count(before_w + 1)
    wait_until(
        lambda: len(session.launched_windows()) > before_os,
        20,
        description="new native OS window after New Tmux Window",
    )
    session.sync_htm_window_recordings()
    session.focus_native_window()


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
    # Hyper: re-select the newest tab so Cmd+W closes the window we just
    # created (WIN3), not an older affinity sibling that still holds markers.
    focus_tab = getattr(session, "focus_newest_tab", None)
    if session.name == "Hyper" and callable(focus_tab):
        try:
            focus_tab()
        except Exception:
            pass
    raise_input = getattr(session, "_raise_mux_for_input", None)
    if callable(raise_input):
        try:
            raise_input(click=True)
        except Exception:
            pass
    session._capturing_text = True
    try:
        if while_writing:
            session.keystroke('"w"', "command down")
        elif session.name == "Hyper" and session.mux == "tmux":
            # Prefer the mux side-channel: Hyper Cmd+W often hits the wrong
            # AX tab. kill-window when the caller expects fewer windows.
            if windows is not None and windows < before_w:
                session.tmux_cmd("kill-window")
            else:
                session.tmux_cmd("kill-pane")
        elif session.name == "Hyper":
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

    def _affinities_match() -> bool:
        dump = session.mux_snapshot(wait=0.3)
        got = htm_gui_parity.parse_affinities_from_dump(dump)
        if got is None:
            return False
        live = sorted({int(pane["wid"]) for pane in htm_gui_parity.parse_panes(dump)})
        flat = sorted({wid for group in got for wid in group})
        if flat != live:
            return False
        # Match iTerm2 Cmd+T partition: one group containing every live window.
        want = [live] if live else []
        return htm_gui_parity.rank_affinity_groups(
            got
        ) == htm_gui_parity.rank_affinity_groups(want)

    deadline = time.time() + 20.0
    while time.time() < deadline and not _affinities_match():
        time.sleep(0.25)
    if not _affinities_match():
        # Stock iTerm2 sometimes lags updating @affinities after an
        # out-of-band kill-window; record whatever it currently persists
        # so Ghostty can be compared to that ground truth.
        dump = session.mux_snapshot(wait=0.3)
        print(
            "WARN: @affinities not fully pruned after close; "
            f"continuing with {htm_gui_parity.parse_affinities_from_dump(dump)!r}",
            flush=True,
        )
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
    # Match iTerm2's default mux window size so --record-video yields the
    # same pre-resize / post-resize segment count (Ghostty otherwise reuses
    # a persisted large frame and never splits the capture).
    if session.supports_native_resize:
        session.resize_front_native_window(570, 462)
        time.sleep(0.35)
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
        session.sync_htm_window_recordings()
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

    sleep_pane = ""
    if session.mux == "tmux":
        # Aim sleep at the pane that still shows CORNER_WIN4 (pane id, not
        # window id — ``send-keys -t @W`` is unreliable here). Clear any
        # half-typed line first. Hyper GUI Enter is flaky on this step even
        # when ``echo CORNER_WIN4`` just succeeded, so always use the mux
        # side-channel for the title probe.
        for pane in htm_gui_parity.parse_panes(session.mux_snapshot(wait=0.4)):
            if tmux_cc.CORNER_WIN4 in (pane.get("body") or ""):
                sleep_pane = f"%{pane['pid']}"
                break
        sk = ["send-keys"]
        if sleep_pane:
            sk.extend(["-t", sleep_pane])
        session.tmux_cmd(*sk, "C-u")
        session.tmux_cmd(*sk, "-l", "sleep 25")
        session.tmux_cmd(*sk, "Enter")
    elif os.name == "nt":
        _submit_command(session, "timeout /t 25")
    else:
        # htm mux: no tmux side-channel. Re-focus the newest tab (WIN4)
        # so sleep does not land on an older affinity sibling.
        if session.name == "Hyper":
            focus_tab = getattr(session, "focus_newest_tab", None)
            if callable(focus_tab):
                try:
                    focus_tab()
                except Exception:
                    pass
            raise_input = getattr(session, "_raise_mux_for_input", None)
            if callable(raise_input):
                try:
                    raise_input(click=True)
                except Exception:
                    pass
        _submit_command(session, "sleep 25")
    title = tmux_cc.TITLE_SLEEP_WIN if os.name == "nt" else tmux_cc.TITLE_SLEEP
    session.wait_window_named(title)
    session.checkpoint("after-title-sleep")
    print(f"OK: automatic-rename window title is {title}", flush=True)
    if session.mux == "tmux":
        sk = ["send-keys"]
        if sleep_pane:
            sk.extend(["-t", sleep_pane])
        session.tmux_cmd(*sk, "C-c")
    else:
        # Ensure Ctrl+C hits the sleep pane (newest WIN4 tab). Do not
        # click first: a mouse click can select text in Hyper, and then
        # Ctrl+C copies instead of sending SIGINT to the shell.
        if session.name == "Hyper":
            focus_tab = getattr(session, "focus_newest_tab", None)
            if callable(focus_tab):
                try:
                    focus_tab()
                except Exception:
                    pass
            raise_input = getattr(session, "_raise_mux_for_input", None)
            if callable(raise_input):
                try:
                    raise_input(click=False)
                except Exception:
                    pass
        session.keystroke('"c"', "control down")
    time.sleep(0.4)
    if session.mux != "tmux":
        # Wait until automatic-rename drops ``sleep`` so the next split is on
        # an idle WIN4 shell, matching the tmux side-channel interrupt.
        wait_until(
            lambda: tmux_cc.TITLE_SLEEP not in session.mux_window_names()
            and (
                os.name != "nt"
                or tmux_cc.TITLE_SLEEP_WIN not in session.mux_window_names()
            ),
            10.0,
            description="sleep title cleared after Ctrl+C",
        )
    _assert_session_alive(session, "after interrupting sleep")

    # Mux side-channel sleep does not move Hyper focus; select the sleep
    # pane and raise a follower so Cmd+D splits it (not a spurious new
    # window from the gateway plate).
    if sleep_pane:
        session.tmux_cmd("select-pane", "-t", sleep_pane)
    if session.name == "Hyper":
        focus_tab = getattr(session, "focus_newest_tab", None)
        if callable(focus_tab):
            try:
                focus_tab()
            except Exception:
                pass
        raise_input = getattr(session, "_raise_mux_for_input", None)
        if callable(raise_input):
            try:
                raise_input(click=True)
            except Exception:
                pass

    _split_horizontal(session)
    _start_writer(session, tmux_cc.WRTICKP)
    session.checkpoint("after-writer-pane")
    _kill_focused(session, panes=4, windows=3, while_writing=True)
    # Hyper Cmd+W can leave focus on an older tab; land back on the window
    # that still shows CORNER_WIN4 (surviving half of the writer split).
    if session.name == "Hyper":
        focus_tab = getattr(session, "focus_newest_tab", None)
        if callable(focus_tab):
            try:
                focus_tab()
            except Exception:
                pass
        raise_input = getattr(session, "_raise_mux_for_input", None)
        if callable(raise_input):
            try:
                raise_input(click=True)
            except Exception:
                pass
        if session.mux == "tmux":
            for pane in htm_gui_parity.parse_panes(session.mux_snapshot(wait=0.3)):
                if tmux_cc.CORNER_WIN4 in (pane.get("body") or ""):
                    session.tmux_cmd("select-pane", "-t", f"%{pane['pid']}")
                    break
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


# Markers for the multi-OS-window affinities suite (A = first OS window, B = second).
AFF_A0 = "AFF_A0"
AFF_A1 = "AFF_A1"
AFF_A2 = "AFF_A2"
AFF_B0 = "AFF_B0"
AFF_B1 = "AFF_B1"
AFF_AFTER_REATTACH = "AFF_AFTER_REATTACH"


def run_gui_affinities(session: GuiTerminalSession) -> None:
    """Multiple OS windows × tabs; tab into a non-newest window; detach/reattach."""
    session.start(session.multiplexer_command())
    session.wait_init()
    session.after_attach()
    _wait_for_native_tab(session)
    session.wait_native_mux_windows(1, timeout=15)
    print(f"OK: attached to {session.name} mux={session.mux}", flush=True)
    session.begin_htm_window_recording("affinities")
    try:
        _run_gui_affinities_body(session)
    finally:
        session.end_htm_window_recording()


def _run_gui_affinities_body(session: GuiTerminalSession) -> None:
    session._marker_host_window = {}
    session.focus_native_window()
    session.wait_mux_window_count(1)
    _echo_marker(session, AFF_A0)
    if session._front_native is not None:
        session._marker_host_window[AFF_A0] = dict(session._front_native)
    session.checkpoint("aff-after-first-window")
    print("OK: OS window A created", flush=True)

    _new_window(session)
    _echo_marker(session, AFF_A1)
    if session._front_native is not None:
        session._marker_host_window[AFF_A1] = dict(session._front_native)
        session._marker_host_window.setdefault(
            AFF_A0, dict(session._front_native)
        )
    session.checkpoint("aff-after-tab-on-a")
    session.wait_native_mux_windows(1)
    print("OK: Cmd+T added a tab on OS window A", flush=True)

    before_ids = {
        pane["wid"]
        for pane in htm_gui_parity.parse_panes(session.mux_snapshot(wait=0.3))
    }
    _new_os_window(session)
    after_panes = htm_gui_parity.parse_panes(session.mux_snapshot(wait=0.3))
    after_ids = {pane["wid"] for pane in after_panes}
    new_ids = after_ids - before_ids
    if len(new_ids) != 1:
        fail(f"expected one new tmux window after New Tmux Window, got {new_ids}")
    new_b = "@" + next(iter(new_ids))
    # Typing follows the focused GUI OS window (newest after New Tmux Window),
    # not a parallel-client select-window.
    session.focus_native_window()
    time.sleep(0.4)
    _echo_marker(session, AFF_B0)
    if session._front_native is not None:
        session._marker_host_window[AFF_B0] = dict(session._front_native)
    b0_wids = {
        pane["wid"]
        for pane in htm_gui_parity.parse_panes(session.mux_snapshot(wait=0.3))
        if AFF_B0 in (pane.get("body") or "")
    }
    if b0_wids != {new_b.lstrip("@")}:
        fail(
            f"AFF_B0 should live only in new OS window {new_b}, "
            f"found in @{sorted(b0_wids)}"
        )
    session.wait_native_mux_windows(2)
    session.checkpoint("aff-after-second-os-window")
    print(f"OK: New Tmux Window opened OS window B ({new_b})", flush=True)

    # Arbitrary order: tab into A's group while B is newest.
    _focus_os_window_showing(session, AFF_A0)
    time.sleep(0.3)
    _new_window(session)
    _echo_marker(session, AFF_A2)
    session.checkpoint("aff-after-tab-on-older-a")
    print("OK: Cmd+T on OS window A (not newest B) added a tab", flush=True)

    _focus_os_window_showing(session, AFF_B0)
    time.sleep(0.3)
    _new_window(session)
    _echo_marker(session, AFF_B1)
    session.checkpoint("aff-after-tab-on-b")
    session.wait_native_mux_windows(2)
    print("OK: two OS windows, each with multiple tabs", flush=True)

    if not session.supports_detach:
        print(f"SKIP detach/reattach on {session.name}", flush=True)
        return

    # Settle so iTerm can flush @affinities before Esc.
    time.sleep(1.0)
    pre = session.mux_snapshot(wait=0.5)
    pre_aff = htm_gui_parity.layout_affinities(pre)
    pre_markers = {AFF_A0, AFF_A1, AFF_A2, AFF_B0, AFF_B1}
    missing = [m for m in pre_markers if m not in pre]
    if missing:
        fail(f"pre-detach dump missing markers {missing}")
    print(
        f"OK: pre-detach affinities {htm_gui_parity.affinities_json(pre_aff)}",
        flush=True,
    )

    session.detach_client()
    session.reattach_client()
    session.wait_native_mux_windows(2, timeout=25)
    session.focus_native_window()
    time.sleep(0.4)
    _echo_marker(session, AFF_AFTER_REATTACH)
    session.checkpoint("aff-after-reattach")

    post = session.mux_snapshot(wait=0.5)
    post_aff = htm_gui_parity.layout_affinities(post)
    if post_aff != pre_aff:
        fail(
            "affinities after reattach diverged from pre-detach: "
            f"pre={htm_gui_parity.affinities_json(pre_aff)} "
            f"post={htm_gui_parity.affinities_json(post_aff)}"
        )
    for marker in (AFF_A0, AFF_A1, AFF_A2, AFF_B0, AFF_B1):
        if marker not in post:
            fail(f"after reattach missing pane marker {marker}")
    if len(session.launched_windows()) < 2:
        fail("after reattach expected 2 native mux OS windows")
    print(
        f"OK: reattach restored affinities {htm_gui_parity.affinities_json(post_aff)} "
        f"and {len(session.launched_windows())} OS windows",
        flush=True,
    )


def run_affinities_suite(session: GuiTerminalSession) -> None:
    run_gui_affinities(session)
    _assert_session_alive(session, "after affinities test")
    session.shutdown_multiplexer()


def normalize_gateway_text(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def assert_control_mode_attached(session: GuiTerminalSession) -> None:
    """iTerm2-compatible attach: command menu on gateway + native mux window.

    The control plate must live in its own OS window. Mux panes as tabs on
    the gateway (Ghostty's old behavior) is a hard failure — iTerm2 never
    does that.
    """
    wait_until(
        lambda: EXPECTED_TMUX_COMMAND_MENU
        in normalize_gateway_text(session.gateway_text()),
        15,
        description=f"{session.name} iTerm2-compatible tmux command menu",
    )
    wait_until(
        lambda: session.window_count() >= 2,
        15,
        description=(
            f"{session.name} separate OS window for the mux session "
            "(not a tab on the control-plane window)"
        ),
    )
    # Do not re-run remember_gateway_windows here: Hyper mux OS windows often
    # share the generic AX title ``Hyper``, so a late snapshot would mark them
    # as gateways and launched_windows() would go empty.
    natives = session.launched_windows()
    if not natives:
        fail(
            f"{session.name}: control plane and mux session share one OS window "
            "as tabs; tmux -CC requires a separate native window for panes"
        )
    # Gateway window must stay at a single tab (the control plate only).
    if session.tab_count() > 1 and session.window_count() < 2:
        fail(
            f"{session.name}: gateway hosts {session.tab_count()} tabs; "
            "mux panes must not share the control-plane window"
        )
    try:
        session.focus_native_window()
    except Exception:
        pass
    print(
        f"OK: {session.name} control plane menu + native mux surface ready",
        flush=True,
    )


def run_control_plane_suite(session: GuiTerminalSession) -> None:
    """Verify Esc/X/L/C control-plane UX and that the mux server survives."""
    session.start(session.multiplexer_command())
    session.wait_init()
    session.after_attach()
    assert_control_mode_attached(session)
    session.begin_htm_window_recording("control-plane")
    try:
        session.focus_gateway()
        session.keystroke('"l"')
        wait_until(
            lambda: "tmux logging enabled"
            in normalize_gateway_text(session.gateway_text()),
            10,
            description=f"{session.name} tmux protocol logging enabled",
        )

        session.keystroke('"c"')
        time.sleep(0.4)
        session.keystroke('"new-window"')
        session.key_code(36)
        session.wait_mux_window_count(2, timeout=15)
        wait_until(
            lambda: (
                "> new-window" in normalize_gateway_text(session.gateway_text())
                and "< %begin" in normalize_gateway_text(session.gateway_text())
            ),
            10,
            description=f"{session.name} displayed raw tmux protocol traffic",
        )
        session.sync_htm_window_recordings()
        print(
            f"OK: C ran new-window through the {session.name} tmux command prompt",
            flush=True,
        )

        session.focus_gateway()
        session.keystroke('"l"')
        wait_until(
            lambda: "tmux logging disabled"
            in normalize_gateway_text(session.gateway_text()),
            10,
            description=f"{session.name} tmux protocol logging disabled",
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
                description=f"tmux server survived {session.name} force quit",
            )
        else:
            wait_until(
                lambda: bool(pids_named("htmd")) and ipc_path().exists(),
                15,
                description=f"htmd survived {session.name} force quit",
            )
        wait_until(
            lambda: session.window_count() == 1 and session.tab_count() <= 1,
            15,
            description=f"{session.name} force quit closed native mux tabs/windows",
        )
        print(f"OK: X force-quit the {session.mux} client only", flush=True)
    finally:
        session.end_htm_window_recording()
    session.shutdown_multiplexer()


SUITES: dict[str, Callable[[GuiTerminalSession], None]] = {
    "layout": run_layout_suite,
    "stress": run_stress_suite,
    "corners": run_corners_suite,
    "affinities": run_affinities_suite,
    "control-plane": run_control_plane_suite,
}
SUITE_ORDER = ("layout", "stress", "corners", "affinities", "control-plane")

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
        help="layout, stress, corners, affinities, control-plane, "
        "comma-separated names, or all "
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
