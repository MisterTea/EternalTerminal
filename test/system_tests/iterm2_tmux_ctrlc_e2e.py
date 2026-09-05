#!/usr/bin/env python3
"""Autonomous macOS visual tests: Ctrl+C through tmux and tmux -CC over et.

Launches a private iTerm2 prefs suite, a throwaway sshd (so we do not need
passwordless localhost SSH), and etserver. iTerm2 runs ``et`` which starts
tmux or tmux -CC on the far side of Eternal Terminal. A numbered flood is
driven with System Events; the window is screenshotted and the visible
buffer is copied with Cmd+A / Cmd+C / ``pbpaste``.

The test is not faithful unless data is actually dropped: it requires the
etserver log to record a flush of at least 64KiB and the highest ``FLOOD_``
sequence on screen to lag the producer by at least 1000 lines.

Skip (exit 77) off macOS, without a GUI session, without iTerm2/tmux/et, or
if a private sshd cannot be started. Not registered with default CTest:

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
import socket
import subprocess
import sys
import tempfile
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
GATEWAY_RE = re.compile(
    r"(?:tmux mode started|Command Menu|%extended-output|%output %)"
)
FLOOD_RE = re.compile(r"FLOOD_(\d+)")
FLUSH_RE = re.compile(r"Flushed (\d+) bytes of terminal output on interrupt")
ATTACHED = "ET_CTRLC_ATTACHED"
AFTER = "ET_CTRLC_AFTER"
MIN_PRODUCED = 20000
MIN_LOST_LINES = 1000
MIN_FLUSH_BYTES = 64 * 1024
FLOOD_SCRIPT = r"""
import os
import sys
path = sys.argv[1]
n = 0

def write_count(value):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(str(value))
    os.replace(tmp, path)

try:
    while True:
        n += 1
        sys.stdout.write("FLOOD_%08d\n" % n)
        if n % 128 == 0:
            sys.stdout.flush()
            write_count(n)
except (KeyboardInterrupt, BrokenPipeError):
    write_count(n)
    sys.exit(0)
"""


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


def unused_port() -> int:
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


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


def find_build_bin(name: str, build_dir: Path) -> Path:
    path = build_dir / name
    if path.is_file():
        return path
    skip(f"{name} is not built at {path}")


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
        "AutoHideTmuxClientSession": True,
        "UnlimitedScrollback": True,
    }
    for key, enabled in bools.items():
        subprocess.run(
            ["defaults", "write", SUITE, key, "-bool", "true" if enabled else "false"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


def read_count(path: Path) -> int:
    try:
        return int(path.read_text(encoding="utf-8").strip())
    except (OSError, ValueError):
        return 0


def max_flood_seen(text: str) -> int:
    found = [int(match.group(1)) for match in FLOOD_RE.finditer(text)]
    return max(found) if found else 0


def flushed_bytes(log_dir: Path) -> int:
    total = 0
    if not log_dir.is_dir():
        return 0
    for path in log_dir.rglob("*"):
        if not path.is_file():
            continue
        try:
            body = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for match in FLUSH_RE.finditer(body):
            total += int(match.group(1))
    return total


def log_dir_tail(log_dir: Path, limit: int = 4000) -> str:
    parts = []
    if not log_dir.is_dir():
        return ""
    for path in sorted(log_dir.rglob("*")):
        if not path.is_file():
            continue
        try:
            body = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        parts.append(f"\n--- {path.name} ---\n{body[-limit:]}")
    return "".join(parts)


class PrivateSshd:
    def __init__(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="et_tmux_sshd_"))
        self.port = unused_port()
        self.proc: Optional[subprocess.Popen] = None
        self.key = self.work / "user"

    def start(self) -> None:
        subprocess.check_call(
            ["ssh-keygen", "-t", "ed25519", "-f", str(self.work / "host"), "-N", ""],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        subprocess.check_call(
            ["ssh-keygen", "-t", "ed25519", "-f", str(self.key), "-N", ""],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        auth = self.work / "authorized_keys"
        auth.write_text((self.work / "user.pub").read_text())
        os.chmod(auth, 0o600)
        cfg = self.work / "sshd_config"
        cfg.write_text(
            "\n".join(
                [
                    f"Port {self.port}",
                    "ListenAddress 127.0.0.1",
                    f"HostKey {self.work / 'host'}",
                    f"PidFile {self.work / 'sshd.pid'}",
                    f"AuthorizedKeysFile {auth}",
                    "PasswordAuthentication no",
                    "KbdInteractiveAuthentication no",
                    "ChallengeResponseAuthentication no",
                    "UsePAM no",
                    "StrictModes no",
                    "PermitRootLogin no",
                    "",
                ]
            )
        )
        sshd = shutil.which("sshd") or "/usr/sbin/sshd"
        if not Path(sshd).is_file():
            skip("sshd is not available for the private loopback listener")
        self.proc = subprocess.Popen(
            [sshd, "-f", str(cfg), "-D", "-e"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            wait_until(
                lambda: self.proc is not None and self.proc.poll() is None,
                3,
                description="private sshd start",
            )
        except WaitTimeout:
            skip("private sshd failed to start")
        probe = subprocess.run(
            [
                "ssh",
                "-i",
                str(self.key),
                "-o",
                "IdentitiesOnly=yes",
                "-o",
                "StrictHostKeyChecking=no",
                "-o",
                "UserKnownHostsFile=/dev/null",
                "-o",
                "BatchMode=yes",
                "-p",
                str(self.port),
                "127.0.0.1",
                "echo ok",
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
        if probe.returncode != 0 or "ok" not in probe.stdout:
            skip(f"private sshd rejected the test key: {probe.stderr.strip()}")

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)
        shutil.rmtree(self.work, ignore_errors=True)


class EtServer:
    def __init__(self, etserver: Path) -> None:
        self.etserver = etserver
        self.port = unused_port()
        self.work = Path(tempfile.mkdtemp(prefix="et_tmux_server_"))
        self.fifo = self.work / "etserver.idpasskey.fifo"
        self.log_dir = self.work / "logs"
        self.log_dir.mkdir()
        self.proc: Optional[subprocess.Popen] = None

    def start(self) -> None:
        self.proc = subprocess.Popen(
            [
                str(self.etserver),
                "--port",
                str(self.port),
                "--serverfifo",
                str(self.fifo),
                "-l",
                str(self.log_dir),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            wait_until(self._listening, 8, description="etserver listen")
        except WaitTimeout:
            fail(f"etserver did not listen on 127.0.0.1:{self.port}")

    def _listening(self) -> bool:
        if self.proc is None or self.proc.poll() is not None:
            return False
        sock = socket.socket()
        sock.settimeout(0.2)
        try:
            sock.connect(("127.0.0.1", self.port))
            return True
        except OSError:
            return False
        finally:
            sock.close()

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)
        shutil.rmtree(self.work, ignore_errors=True)


class ITermTmuxSession:
    def __init__(
        self,
        app: Path,
        tmux: Path,
        et: Path,
        etterminal: Path,
        sshd: PrivateSshd,
        server: EtServer,
        control_mode: bool,
        flood_script: Path,
        count_path: Path,
    ):
        self.app = app
        self.tmux = tmux
        self.et = et
        self.etterminal = etterminal
        self.sshd = sshd
        self.server = server
        self.control_mode = control_mode
        self.flood_script = flood_script
        self.count_path = count_path
        self.socket = f"et_tmux_ctrlc_{os.getpid()}_{'cc' if control_mode else 'tty'}"
        self.proc: Optional[subprocess.Popen] = None
        self.preexisting_iterm = set(self._iterm_pids())
        self._capturing_text = False
        self.label = "tmux-cc" if control_mode else "tmux"
        self.native_window = 1
        self.client_log = server.work / f"client-{self.label}"
        self.client_log.mkdir(exist_ok=True)

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

    def list_window_names(self) -> list[str]:
        raw = self.osascript_pid(
            "set AppleScript's text item delimiters to ASCII character 10\n"
            "    return (name of windows) as text"
        )
        return [name for name in raw.splitlines() if name.strip()]

    def is_gateway_title(self, name: str) -> bool:
        n = name.lower()
        return (
            "-cc" in n
            or " -c" in n
            or n.endswith("-c")
            or n.startswith("exec /")
            or "tmux -l" in n
        )

    @staticmethod
    def is_native_pane(name: str) -> bool:
        # iTerm2 titles the nested pane "… [exec (et)]" or, when truncated,
        # "… [exec]". The gateway window contains "-CC" and must be skipped.
        return "[exec" in name and "-CC" not in name

    def output_window_index(self) -> int:
        names = self.list_window_names()
        for index, name in enumerate(names, 1):
            if self.is_native_pane(name):
                return index
        return 1

    def raise_window(self, index: int) -> None:
        try:
            self.osascript_pid(
                f'perform action "AXRaise" of window {index}\n'
                "    set frontmost to true"
            )
        except subprocess.CalledProcessError:
            self.focus()
        time.sleep(0.12)

    def discover_native_window(self, marker: Optional[str] = None) -> None:
        """Raise the iTerm2 pane, not the tmux -CC gateway."""
        if not self.control_mode:
            self.native_window = 1
            return
        deadline = time.time() + 8
        last_names: list[str] = []
        while time.time() < deadline:
            last_names = self.list_window_names()
            print(f"iTerm2 windows={last_names}", flush=True)
            for index, name in enumerate(last_names, 1):
                if self.is_native_pane(name):
                    self.raise_window(index)
                    self.native_window = 1
                    print(
                        f"raised native iTerm2 window {index}: {name!r}",
                        flush=True,
                    )
                    return
            time.sleep(0.25)
        fail(f"no native tmux pane window in {last_names}")

    def focus_output_window(self) -> None:
        if self.window_count() < 1:
            self.focus()
            return
        self.raise_window(self.output_window_index())

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
        self.focus_output_window()
        self.keystroke(applescript_quote(text))
        self.key_code(36)

    def window_count(self) -> int:
        try:
            return int(self.osascript_pid("get count of windows").strip())
        except (ValueError, subprocess.CalledProcessError):
            return 0

    def window_frame(self) -> tuple[float, float, float, float]:
        index = self.output_window_index()
        out = self.osascript_pid(
            f"set p to position of window {index}\n"
            f"set s to size of window {index}\n"
            'return (item 1 of p as text) & "," & (item 2 of p as text) & "," & '
            '(item 1 of s as text) & "," & (item 2 of s as text)'
        ).strip()
        x, y, width, height = [float(part) for part in out.split(",")]
        return x, y, width, height

    def screenshot(self, name: str) -> Path:
        out_dir = CAPTURE_DIR / self.label
        out_dir.mkdir(parents=True, exist_ok=True)
        path = out_dir / f"{name}.png"
        self.focus_output_window()
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
        self.focus_output_window()
        self._capturing_text = True
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
        remote = (
            f"exec {shlex.quote(str(self.tmux))} -L {shlex.quote(self.socket)} "
            f"-f /dev/null {flags}new-session -x 80 -y 24"
        )
        launcher = self.server.work / f"launch-{self.label}.sh"
        lines = [
            "#!/bin/sh",
            "exec " + " ".join(shlex.quote(part) for part in [
                str(self.et),
                "--serverfifo",
                str(self.server.fifo),
                "--terminal-path",
                str(self.etterminal),
                "--logdir",
                str(self.client_log),
                "--ssh-option",
                f"Port={self.sshd.port}",
                "--ssh-option",
                f"IdentityFile={self.sshd.key}",
                "--ssh-option",
                "IdentitiesOnly=yes",
                "--ssh-option",
                "StrictHostKeyChecking=no",
                "--ssh-option",
                "UserKnownHostsFile=/dev/null",
                "--ssh-option",
                "BatchMode=yes",
                "-c",
                remote,
                f"127.0.0.1:{self.server.port}",
            ]),
            "",
        ]
        launcher.write_text("\n".join(lines))
        os.chmod(launcher, 0o755)
        command = str(launcher)
        print(f"iTerm2 command={command}\n{launcher.read_text()}", flush=True)
        env = os.environ.copy()
        env["PATH"] = f"{self.tmux.parent}:{self.et.parent}:{env.get('PATH', '')}"
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
                lambda: self.window_count() > 0, 30, description="iTerm2 window"
            )
        except WaitTimeout as exc:
            fail(f"timed out waiting for {exc}")
        self.focus()
        try:
            wait_until(
                lambda: bool(self.et_client_pids()),
                20,
                description="et client process",
            )
        except WaitTimeout:
            fail(
                "iTerm2 did not start et; pids with fifo: "
                + subprocess.run(
                    ["pgrep", "-af", str(self.server.fifo)],
                    capture_output=True,
                    text=True,
                ).stdout
            )
        time.sleep(1.5)
        if self.control_mode:
            deadline = time.time() + 12
            while time.time() < deadline and self.window_count() < 1:
                time.sleep(0.2)
            time.sleep(1.0)

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
        deadline = time.time() + 5
        while time.time() < deadline:
            leftover = [
                pid
                for pid in self._iterm_pids()
                if pid not in self.preexisting_iterm
                and (SUITE in self._pid_command(pid) or str(self.app) in self._pid_command(pid))
            ]
            if not leftover:
                break
            time.sleep(0.2)

    def et_client_pids(self) -> list[int]:
        try:
            out = subprocess.check_output(
                ["pgrep", "-f", str(self.server.fifo)],
                text=True,
            )
        except subprocess.CalledProcessError:
            return []
        pids = []
        for line in out.split():
            try:
                pid = int(line)
            except ValueError:
                continue
            cmd = self._pid_command(pid)
            head = cmd.split()[0] if cmd.split() else ""
            base = os.path.basename(head)
            if base in ("etserver", "etterminal", "sshd", "ssh", "Python", "python3"):
                continue
            if base == "et" or head.endswith("/et"):
                pids.append(pid)
        return pids

    def pause_et_client(self) -> list[int]:
        pids = self.et_client_pids()
        if not pids:
            fail("could not find the et client process to pause (no backlog would form)")
        for pid in pids:
            os.kill(pid, signal.SIGSTOP)
        print(f"SIGSTOP et client pids={pids}", flush=True)
        return pids

    def resume_et_client(self, pids: list[int]) -> None:
        for pid in pids:
            try:
                os.kill(pid, signal.SIGCONT)
            except OSError:
                pass
        print(f"SIGCONT et client pids={pids}", flush=True)

    def et_stdin_tty(self, pid: int) -> str:
        try:
            out = subprocess.check_output(
                ["lsof", "-a", "-p", str(pid), "-d", "0", "-Fn"],
                text=True,
                stderr=subprocess.DEVNULL,
            )
        except (subprocess.CalledProcessError, FileNotFoundError) as exc:
            fail(f"lsof et stdin failed: {exc}")
        for line in out.splitlines():
            if line.startswith("n/") and "tty" in line:
                return line[1:]
        fail(f"et pid {pid} stdin is not a tty: {out!r}")

    def queue_interrupt(self, pids: list[int]) -> None:
        """Queue Ctrl+C so the server sees it before the backlog drains.

        For tmux -CC, iTerm2 must originate ``send-keys`` itself. Injecting
        that command into et's PTY makes tmux reply ``%begin`` for a command
        iTerm2 did not queue, and iTerm2 tears down tmux mode.
        """
        if self.control_mode:
            self.focus_output_window()
            self.keystroke('"c"', "control down")
            time.sleep(0.15)
            self.keystroke('"c"', "control down")
            for pid in pids:
                tty = self.et_stdin_tty(pid)
                waiting = self.tty_bytes_waiting(tty)
                print(
                    f"queued iTerm2 Ctrl+C for tmux -CC; {tty} has {waiting} bytes waiting",
                    flush=True,
                )
            return
        payload = b"\n\x03"
        for pid in pids:
            tty = self.et_stdin_tty(pid)
            fd = os.open(tty, os.O_WRONLY | os.O_NONBLOCK)
            try:
                os.write(fd, payload)
            finally:
                os.close(fd)
            print(f"queued interrupt on {tty} for pid {pid}", flush=True)

    def tty_bytes_waiting(self, tty: str) -> int:
        try:
            fd = os.open(tty, os.O_RDONLY | os.O_NONBLOCK)
        except OSError:
            return -1
        try:
            import array
            import fcntl
            import termios

            buf = array.array("i", [0])
            fcntl.ioctl(fd, termios.FIONREAD, buf)
            return int(buf[0])
        except OSError:
            return -1
        finally:
            os.close(fd)

    def iterm_write_ctrl_c(self) -> None:
        """Send Ctrl+C through iTerm2's session write path (tmux send-keys)."""
        script = r'''
tell application "iTerm2"
  repeat with w in windows
    repeat with t in tabs of w
      repeat with s in sessions of t
        set sessionName to name of s
        if sessionName contains "exec (et)" and sessionName does not contain "-CC" then
          tell s
            write text (ASCII character 3) without newline
          end tell
        end if
      end repeat
    end repeat
  end repeat
end tell
'''
        try:
            subprocess.run(
                ["osascript", "-e", script],
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception:
            pass


def run_ctrl_c_scenario(session: ITermTmuxSession) -> None:
    mode = "tmux -CC" if session.control_mode else "tmux"
    print(f"=== {mode} over et: numbered flood, Ctrl+C, assert dropped data ===", flush=True)
    session.start()
    session.keystroke('"c"', "control down")
    time.sleep(0.4)
    session.dump_visible("after-launch")
    session.type_line(f"echo {ATTACHED}")
    session.wait_visible(ATTACHED, 30, f"{ATTACHED} after et+tmux attach")
    session.discover_native_window(ATTACHED)
    session.dump_visible("after-attach", require=ATTACHED)

    try:
        session.count_path.unlink()
    except OSError:
        pass
    session.type_line(f"python3 {session.flood_script} {session.count_path}")
    peak = 0

    def flood_started() -> bool:
        nonlocal peak
        peak = max(peak, read_count(session.count_path))
        return peak >= 2000

    try:
        wait_until(flood_started, 12, interval=0.05, description="flood producer started")
    except WaitTimeout:
        fail(f"{mode}: flood producer only reached {peak} lines")
    paused = session.pause_et_client()
    deadline = time.time() + 2.0
    while time.time() < deadline:
        peak = max(peak, read_count(session.count_path))
        time.sleep(0.05)
    produced_before = max(peak, read_count(session.count_path))
    session.screenshot("during-spam")
    print(f"{mode}: producer reached {produced_before} with et client paused", flush=True)
    if produced_before < MIN_PRODUCED:
        session.resume_et_client(paused)
        fail(
            f"{mode}: backlog only reached {produced_before} lines while et "
            f"was paused (need {MIN_PRODUCED})"
        )
    dropped_before = flushed_bytes(session.server.log_dir)
    interrupt_at = time.time()
    session.queue_interrupt(paused)
    time.sleep(0.25)
    session.keystroke('"c"', "control down")
    session.resume_et_client(paused)
    time.sleep(0.4)
    session.discover_native_window()
    produced = max(produced_before, read_count(session.count_path))
    session.type_line(f"echo {AFTER}")
    session.wait_visible(AFTER, 15, f"{AFTER} after Ctrl+C")
    latency = time.time() - interrupt_at
    after = session.dump_visible("after-ctrl-c", require=AFTER)
    if latency >= 15:
        fail(f"{mode}: prompt marker took {latency:.2f}s after Ctrl+C")

    produced = max(produced, read_count(session.count_path))
    flood_ids = {int(match.group(1)) for match in FLOOD_RE.finditer(after)}
    seen = max(flood_ids) if flood_ids else 0
    delivered = len(flood_ids)
    lost = max(0, produced - delivered)
    dropped = flushed_bytes(session.server.log_dir) - dropped_before
    print(
        f"{mode}: produced={produced} delivered_unique={delivered} "
        f"min_on_screen={min(flood_ids) if flood_ids else 0} "
        f"max_on_screen={seen} lost_lines={lost} flushed_bytes={dropped} "
        f"recover={latency:.2f}s",
        flush=True,
    )
    if dropped < MIN_FLUSH_BYTES:
        fail(
            f"{mode}: etserver flushed {dropped} bytes; need at least "
            f"{MIN_FLUSH_BYTES} so the interrupt actually dropped a backlog."
            f"\n=== etserver ==={log_dir_tail(session.server.log_dir)}"
            f"\n=== et client ==={log_dir_tail(session.client_log)}"
        )
    if lost < MIN_LOST_LINES:
        fail(
            f"{mode}: client still has {delivered} of {produced} numbered lines "
            f"(lost {lost}); a faithful flush must drop at least "
            f"{MIN_LOST_LINES} lines"
        )
    print(f"OK: {mode} dropped {lost} lines / {dropped} bytes in {latency:.2f}s", flush=True)


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
        description="Autonomous iTerm2 visual tests for tmux and tmux -CC Ctrl+C over et"
    )
    parser.add_argument("--iterm2-app")
    parser.add_argument(
        "--build-dir",
        default=str(Path(__file__).resolve().parents[2] / "build"),
        help="Directory containing et/etserver/etterminal",
    )
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
    python3 = shutil.which("python3")
    if not python3:
        skip("python3 is required for the numbered flood producer")
    build_dir = Path(args.build_dir)
    et = find_build_bin("et", build_dir)
    etserver = find_build_bin("etserver", build_dir)
    etterminal = find_build_bin("etterminal", build_dir)
    app = find_iterm_app()
    print(
        f"Using iTerm2={app} tmux={tmux} et={et} etserver={etserver}",
        flush=True,
    )
    CAPTURE_DIR.mkdir(parents=True, exist_ok=True)

    flood_script = Path(tempfile.gettempdir()) / "et_tmux_ctrlc_flood.py"
    flood_script.write_text(FLOOD_SCRIPT)
    os.chmod(flood_script, 0o755)

    sshd = PrivateSshd()
    server = EtServer(etserver)
    sshd.start()
    server.start()
    print(f"loopback sshd={sshd.port} etserver={server.port}", flush=True)

    modes = []
    if args.mode in ("all", "tmux"):
        modes.append(False)
    if args.mode in ("all", "tmux-cc"):
        modes.append(True)

    try:
        for control_mode in modes:
            label = "cc" if control_mode else "tty"
            count_path = Path(tempfile.gettempdir()) / f"et_tmux_ctrlc_count_{os.getpid()}_{label}"
            session = ITermTmuxSession(
                app,
                Path(tmux),
                et,
                etterminal,
                sshd,
                server,
                control_mode,
                flood_script,
                count_path,
            )
            try:
                run_ctrl_c_scenario(session)
            finally:
                session.stop()
    finally:
        server.stop()
        sshd.stop()

    print("PASS: iTerm2 tmux/tmux -CC Ctrl+C visual e2e (data was dropped)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
