"""Loopback CLI regression tests for default saved-session persistence."""

import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest


@unittest.skipUnless(
    sys.platform.startswith(("darwin", "linux")), "requires macOS or Linux"
)
class DefaultPersistenceCliTest(unittest.TestCase):
    timeout = 30

    def setUp(self):
        root = Path(__file__).resolve().parents[2]
        build = Path(os.environ.get("ET_BUILD_DIR", root / "build")).resolve()
        self.et, self.etserver, self.etterminal = [
            build / name for name in ("et", "etserver", "etterminal")
        ]
        missing = [
            str(p) for p in (self.et, self.etserver, self.etterminal) if not p.is_file()
        ]
        if missing:
            self.skipTest("built ET binaries are missing: " + ", ".join(missing))
        self.workspace = Path(tempfile.mkdtemp(prefix="et-persist-", dir="/tmp"))
        self.home, self.fake_bin, self.logs = [
            self.workspace / name for name in ("home", "bin", "logs")
        ]
        for path in (self.home, self.fake_bin, self.logs):
            path.mkdir()
        self.fifo = self.workspace / "registration.sock"
        self.port = self._free_port()
        self.clients, self.servers = [], []
        self.env = os.environ.copy()
        self.env.update(
            HOME=str(self.home),
            TERM="xterm-256color",
            PATH=str(self.fake_bin) + os.pathsep + os.environ.get("PATH", ""),
        )
        self.addCleanup(self._cleanup)
        self._install_fake_ssh()
        self.server = self._start_server()

    @staticmethod
    def _free_port():
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            return sock.getsockname()[1]

    def _install_fake_ssh(self):
        fake = self.fake_bin / "ssh"
        fake.write_text(
            textwrap.dedent(f"""\
            #!{sys.executable}
            import os
            import re
            import sys
            terminal = {str(self.etterminal)!r}
            fifo = {str(self.fifo)!r}
            if len(sys.argv) != 3 or not re.fullmatch(
                    r"[A-Za-z0-9._-]+@127\\.0\\.0\\.1", sys.argv[1]):
                raise SystemExit("unexpected ssh arguments")
            bootstrap = sys.argv[2]
            marker = "echo '"
            end = bootstrap.find("' | ")
            payload = bootstrap[len(marker):end]
            if (end < len(marker) or not re.fullmatch(
                    r"XXX[A-Za-z0-9]{{13}}/[A-Za-z0-9]{{32}}_xterm-256color",
                    payload)):
                raise SystemExit("malformed ET bootstrap")
            expected = ("echo '" + payload + "' | " + terminal +
                        " --verbose=0 --serverfifo=" + fifo)
            if bootstrap != expected:
                raise SystemExit("refusing a non-ET shell command")
            os.execv("/bin/sh", ["sh", "-c", bootstrap +
                                  " --logdir=" + {str(self.logs)!r}])
        """),
            encoding="utf-8",
        )
        fake.chmod(0o700)

    def _wait(self, condition, description, timeout=None):
        deadline = time.monotonic() + (timeout or self.timeout)
        while time.monotonic() < deadline:
            if condition():
                return
            time.sleep(0.1)
        raise AssertionError("timed out waiting for " + description)

    def _port_ready(self):
        try:
            with socket.create_connection(("127.0.0.1", self.port), 0.2):
                return True
        except OSError:
            return False

    def _start_server(self):
        process = subprocess.Popen(
            [
                str(self.etserver),
                "--telemetry=false",
                "--bindip",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--serverfifo=" + str(self.fifo),
                "--logdir",
                str(self.logs),
            ],
            env=self.env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.servers.append(process)
        self._wait(
            lambda: process.poll() is None and self._port_ready(), "loopback etserver"
        )
        return process

    def _client_cmd(self, *extra):
        return [
            str(self.et),
            "--telemetry=false",
            "--serverfifo=" + str(self.fifo),
            "--terminal-path",
            str(self.etterminal),
            "--logdir",
            str(self.logs),
            "--logtostdout",
            "-N",
            *extra,
            "127.0.0.1:" + str(self.port),
        ]

    def _start_client(self, *extra):
        process = subprocess.Popen(
            self._client_cmd(*extra),
            env=self.env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
        self.clients.append(process)
        return process

    def _finish_client(self, process, kill=False, timeout=5):
        if kill and process.poll() is None:
            process.kill()
        try:
            output, _ = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            output, _ = process.communicate()
        return output

    def _session_paths(self):
        directory = self.home / ".et" / "sessions"
        if not directory.is_dir():
            return []
        return sorted(
            p for p in directory.iterdir() if p.is_file() and not p.name.startswith(".")
        )

    def _records(self):
        records = []
        for path in self._session_paths():
            records.append(
                dict(
                    line.split("=", 1)
                    for line in path.read_text().splitlines()
                    if "=" in line
                )
            )
        return records

    def _assert_credentials_hidden(self, output, records):
        self.assertFalse("IDPASSKEY:" in output, "credential marker leaked")
        for record in records:
            self.assertFalse(record["id"] in output, "session id leaked")
            self.assertFalse(record["passkey"] in output, "session passkey leaked")

    def _terminal_tree(self):
        try:
            output = subprocess.check_output(
                ["ps", "-axo", "pid=,ppid=,command="], text=True, errors="replace"
            )
        except (OSError, subprocess.CalledProcessError):
            return set()
        rows = {}
        for line in output.splitlines():
            fields = line.strip().split(None, 2)
            if len(fields) == 3:
                try:
                    rows[int(fields[0])] = (int(fields[1]), fields[2])
                except ValueError:
                    pass
        marker = "--serverfifo=" + str(self.fifo)
        tree = {
            pid
            for pid, (_, command) in rows.items()
            if marker in command
            and Path(command.split(None, 1)[0]).name == "etterminal"
        }
        while True:
            children = {
                pid
                for pid, (parent, _) in rows.items()
                if parent in tree and pid not in tree
            }
            if not children:
                return tree
            tree.update(children)

    def _stop_server(self, process):
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        else:
            process.wait()

    def test_default_records_are_unique_and_no_persist_is_opt_out(self):
        first_ready = self.workspace / "first-ready"
        first = self._start_client("-e", "-c", "touch " + str(first_ready))
        self._wait(
            lambda: len(self._records()) == 1 and first_ready.is_file(),
            "first saved and initialized session",
        )
        second_ready = self.workspace / "second-ready"
        second = self._start_client("-e", "-c", "touch " + str(second_ready))
        self._wait(
            lambda: len(self._records()) == 2 and second_ready.is_file(),
            "second saved and initialized session",
        )
        records = self._records()
        self.assertEqual(
            {r["name"] for r in records}, {p.name for p in self._session_paths()}
        )
        self.assertEqual(len({r["name"] for r in records}), 2)
        self._assert_credentials_hidden(self._finish_client(first, True), records)
        self._assert_credentials_hidden(self._finish_client(second, True), records)

        marker = self.workspace / "no-persist-complete"
        no_persist = self._start_client(
            "--no-persist", "-c", "printf connected > " + str(marker)
        )
        self._wait(
            lambda: marker.is_file() and no_persist.poll() is not None,
            "no-persist command",
        )
        output = self._finish_client(no_persist)
        self.assertEqual(marker.read_text(), "connected")
        self._assert_credentials_hidden(output, records)
        self.assertEqual(len(self._records()), 2)

    def test_attach_survives_client_and_server_restart_then_removes_record(self):
        ready = self.workspace / "restart-ready"
        client = self._start_client("-e", "-c", "touch " + str(ready))
        self._wait(
            lambda: len(self._records()) == 1 and ready.is_file(),
            "saved and initialized session",
        )
        record = self._records()[0]
        self._assert_credentials_hidden(self._finish_client(client, True), [record])
        self._stop_server(self.server)
        self.server = self._start_server()
        marker = self.workspace / "reattached-complete"
        attached = subprocess.run(
            [
                str(self.et),
                "--telemetry=false",
                "--attach",
                record["name"],
                "--logdir",
                str(self.logs),
                "--logtostdout",
                "-N",
                "-c",
                "printf reattached > " + str(marker),
            ],
            env=self.env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=self.timeout,
        )
        self.assertEqual(attached.returncode, 0)
        self.assertTrue(marker.is_file())
        self.assertEqual(marker.read_text(), "reattached")
        self._assert_credentials_hidden(attached.stdout, [record])
        self._wait(lambda: not self._records(), "ended session record removal")

    def _cleanup(self):
        for client in getattr(self, "clients", []):
            self._finish_client(client, True)
        for server in getattr(self, "servers", []):
            self._stop_server(server)
        for pid in sorted(self._terminal_tree(), reverse=True):
            if pid != os.getpid():
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        try:
            self._wait(lambda: not self._terminal_tree(), "fixture cleanup", 5)
        except AssertionError:
            pass
        if hasattr(self, "workspace"):
            shutil.rmtree(self.workspace, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
