#!/usr/bin/env python3

import contextlib
import importlib.util
import io
import json
import os
import stat
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


SCRIPT = Path(__file__).with_name("et-session-recovery.py")
ROOT_SCRIPT = SCRIPT.with_name("et-session-recovery-root.py")
SPEC = importlib.util.spec_from_file_location("et_session_recovery", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


CLIENT_ID = "AbC1234567890123"
PASSKEY = "pAsskey0123456789012345678901234"
BOOTSTRAP_ID = "XXX1234567890123"
BOOTSTRAP_KEY = "bOoTstrap01234567890123456789012"


class SessionRecoveryTest(unittest.TestCase):
    def load_root_helper(self):
        spec = importlib.util.spec_from_file_location("et_root_audit", ROOT_SCRIPT)
        assert spec is not None and spec.loader is not None
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        return helper

    def test_root_failure_diagnostics_do_not_echo_exception_text(self) -> None:
        helper = self.load_root_helper()
        try:
            raise RuntimeError(f"private source: {CLIENT_ID}/{PASSKEY}")
        except RuntimeError as error:
            report = helper.safe_failure(error)
        serialized = json.dumps(report)
        self.assertEqual(report["exception_type"], "RuntimeError")
        self.assertFalse(CLIENT_ID in serialized, "client ID leaked")
        self.assertFalse(PASSKEY in serialized, "passkey leaked")
        self.assertFalse(report["raw_diagnostics_recorded"])

    def test_root_helper_requires_explicit_or_valid_sudo_user(self) -> None:
        helper = self.load_root_helper()
        for environment in ({}, {"SUDO_USER": "root"}):
            with self.subTest(environment=environment):
                with mock.patch.dict(os.environ, environment, clear=True):
                    with self.assertRaises(helper.AuditError) as raised:
                        helper._resolve_account(None)
                self.assertNotIn(PASSKEY, str(raised.exception))

        account = SimpleNamespace(
            pw_name="alice", pw_uid=501, pw_gid=20, pw_dir="/srv/alice"
        )
        with mock.patch.dict(os.environ, {"SUDO_USER": "alice"}, clear=True):
            with mock.patch.object(
                helper.pwd, "getpwnam", return_value=account
            ) as lookup:
                self.assertIs(helper._resolve_account(None), account)
        lookup.assert_called_once_with("alice")

        root_account = SimpleNamespace(
            pw_name="toor", pw_uid=0, pw_gid=0, pw_dir="/root"
        )
        with mock.patch.dict(os.environ, {"SUDO_USER": "toor"}, clear=True):
            with mock.patch.object(helper.pwd, "getpwnam", return_value=root_account):
                with self.assertRaises(helper.AuditError):
                    helper._resolve_account(None)

    def test_root_helper_scans_the_selected_account_home(self) -> None:
        helper = self.load_root_helper()
        account = SimpleNamespace(pw_dir="/srv/alice")
        scanner = mock.Mock()
        scanner.scan_local.return_value.public.return_value = {"scope": "local"}
        with mock.patch.object(helper, "_load_scanner", return_value=scanner):
            report = helper._scan_account(account)
        self.assertEqual(report, {"scope": "local"})
        scanner.scan_local.assert_called_once_with(home=Path("/srv/alice"))

    def test_available_service_state_is_reported_without_raw_output(self) -> None:
        audit = AUDIT.Audit()
        with mock.patch.object(
            AUDIT,
            "_run_capture",
            return_value=(b"ActiveState=active\nSubState=running\n", "read"),
        ):
            AUDIT._scan_services(audit)
        self.assertEqual(len(audit.service_checks), 2)
        for check in audit.service_checks:
            self.assertEqual(check["status"], "available")
            self.assertEqual(check["active_state"], "active")
            self.assertEqual(check["sub_state"], "running")

    def make_home(self, root: Path) -> Path:
        home = root / "home"
        sessions = home / ".et" / "sessions"
        sessions.mkdir(parents=True, mode=0o700)
        os.chmod(home / ".et", 0o700)
        os.chmod(sessions, 0o700)
        return home

    def write_session(self, home: Path, name: str = "saved") -> Path:
        record = home / ".et" / "sessions" / name
        record.write_text(
            "version=1\n"
            f"name={name}\n"
            "host=nas\n"
            "port=2022\n"
            f"id={CLIENT_ID}\n"
            f"passkey={PASSKEY}\n"
            "savedat=1700000000\n"
            "title=terminal\n",
            encoding="utf-8",
        )
        os.chmod(record, 0o600)
        return record

    def test_report_redacts_pairs_and_associates_terminal_filename(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = self.make_home(root)
            self.write_session(home)
            temp_logs = root / "tmp"
            temp_logs.mkdir(mode=0o700)
            log = temp_logs / f"etterminal-eric-{CLIENT_ID}-1700000000.log"
            log.write_text(f"IDPASSKEY:{CLIENT_ID}/{PASSKEY}\n", encoding="utf-8")
            os.chmod(log, 0o600)

            with mock.patch.dict(os.environ, {"TMPDIR": str(temp_logs)}):
                audit = AUDIT.scan_local(
                    home, include_processes=False, include_services=False
                )
            report = audit.public()
            serialized = json.dumps(report, sort_keys=True)
            rendered = AUDIT._render_text(report)

            self.assertNotIn(CLIENT_ID, serialized)
            self.assertNotIn(PASSKEY, serialized)
            self.assertNotIn(CLIENT_ID, rendered)
            self.assertNotIn(PASSKEY, rendered)
            self.assertEqual(report["summary"]["complete_pairs"], 1)
            candidate = report["candidates"][0]
            self.assertTrue(candidate["complete"])
            self.assertTrue(candidate["eligible_for_import"])
            self.assertGreater(candidate["matched_id_only_evidence"], 0)
            self.assertIn("local_session_store", candidate["sources"])
            self.assertIn("etterminal_log", candidate["sources"])

    def test_bootstrap_pair_is_complete_but_unverified(self) -> None:
        audit = AUDIT.Audit()
        audit.ingest_text(
            f"echo '{BOOTSTRAP_ID}/{BOOTSTRAP_KEY}_xterm-256color'",
            "shell_history",
            "history_command",
        )
        report = audit.public()
        serialized = json.dumps(report, sort_keys=True)

        self.assertNotIn(BOOTSTRAP_ID, serialized)
        self.assertNotIn(BOOTSTRAP_KEY, serialized)
        self.assertEqual(report["summary"]["bootstrap_unverified_pairs"], 1)
        self.assertFalse(report["candidates"][0]["eligible_for_import"])

    def test_symlink_and_fifo_are_not_opened(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = self.make_home(root)
            real = self.write_session(home)
            link = home / ".et" / "sessions" / "etterminal-secret-link"
            link.symlink_to(real)
            fifo = home / ".et" / "sessions" / "etserver-secret-fifo"
            os.mkfifo(fifo, 0o600)

            audit = AUDIT.Audit()
            AUDIT._scan_session_store(Path(os.path.realpath(home)), audit)
            report = audit.public()
            source = report["sources"]["local_session_store"]

            self.assertEqual(report["summary"]["complete_pairs"], 1)
            self.assertGreaterEqual(source["skipped"], 2)
            self.assertNotIn("secret", json.dumps(report))

    def test_import_requires_confirmation_and_writes_private_record(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            recovery = root / "recovery-home"
            audit = AUDIT.Audit()
            audit.add_pair(CLIENT_ID, PASSKEY, "local_session_store", "session_store")
            label = audit.public()["candidates"][0]["label"]

            with self.assertRaises(AUDIT.AuditError):
                AUDIT.import_candidate(
                    audit,
                    label,
                    "recovered",
                    "nas",
                    2022,
                    str(recovery),
                    confirm=lambda _prompt: "no",
                )
            self.assertFalse(recovery.exists())

            AUDIT.import_candidate(
                audit,
                label,
                "recovered",
                "nas",
                2022,
                str(recovery),
                confirm=lambda _prompt: f"IMPORT {label}",
            )
            record = recovery / ".et" / "sessions" / "recovered"
            self.assertTrue(record.is_file())
            self.assertEqual(stat.S_IMODE(record.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE((recovery / ".et").stat().st_mode), 0o700)
            self.assertEqual(
                stat.S_IMODE((recovery / ".et" / "sessions").stat().st_mode), 0o700
            )
            contents = record.read_text(encoding="utf-8")
            self.assertIn(f"id={CLIENT_ID}", contents)
            self.assertIn(f"passkey={PASSKEY}", contents)
            backup = recovery / ".et" / "recovery-backups" / "recovered"
            self.assertEqual(backup.read_bytes(), record.read_bytes())
            self.assertEqual(stat.S_IMODE(backup.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(backup.parent.stat().st_mode), 0o700)
            self.assertNotEqual(backup.stat().st_ino, record.stat().st_ino)
            record.unlink()
            self.assertTrue(backup.is_file())

    def test_import_rejects_unverified_bootstrap_without_override(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            audit = AUDIT.Audit()
            audit.add_pair(BOOTSTRAP_ID, BOOTSTRAP_KEY, "shell_history")
            label = audit.public()["candidates"][0]["label"]
            with self.assertRaises(AUDIT.AuditError):
                AUDIT.import_candidate(
                    audit,
                    label,
                    "recovered",
                    "nas",
                    2022,
                    str(Path(temporary) / "recovery"),
                    confirm=lambda _prompt: f"IMPORT {label}",
                )

    def test_remote_result_is_sanitized_json_and_uses_stdin_source(self) -> None:
        remote_report = AUDIT.Audit("local").public()
        completed = mock.Mock(
            returncode=0, stdout=json.dumps(remote_report).encode(), stderr=b""
        )
        with mock.patch.object(AUDIT.subprocess, "run", return_value=completed) as run:
            result = AUDIT._remote_audit("nas", ["-oBatchMode=yes"])
        self.assertEqual(result["scope"], "remote")
        command = run.call_args.args[0]
        self.assertEqual(command[:2], ["ssh", "-oBatchMode=yes"])
        self.assertEqual(command[-3:], ["python3", "-", "--remote-child"])
        self.assertIsInstance(run.call_args.kwargs["input"], bytes)

    def test_remote_raw_pair_is_rejected_before_rendering(self) -> None:
        remote_report = AUDIT.Audit("local").public()
        remote_report["unexpected"] = f"{CLIENT_ID}/{PASSKEY}"
        completed = mock.Mock(
            returncode=0,
            stdout=json.dumps(remote_report).encode(),
            stderr=b"",
        )
        with mock.patch.object(AUDIT.subprocess, "run", return_value=completed):
            with self.assertRaises(AUDIT.AuditError):
                AUDIT._remote_audit("nas", [])

    def test_remote_split_secrets_and_free_text_are_rejected(self) -> None:
        for extra in (
            {"identifier": CLIENT_ID, "secret_value": PASSKEY},
            {"warnings": [PASSKEY]},
            {"sources": {PASSKEY: {"files_read": 1}}},
        ):
            report = AUDIT.Audit().public()
            report.update(extra)
            self.assertTrue(AUDIT._report_has_sensitive_fields(report))

    def test_import_rejects_a_parent_writable_by_other_accounts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary).resolve() / "shared"
            parent.mkdir(mode=0o777)
            parent.chmod(0o777)
            recovery = parent / "new-home"
            with self.assertRaises(AUDIT.AuditError):
                AUDIT._write_private_record(
                    recovery, "recovered", "nas", 2022, CLIENT_ID, PASSKEY
                )
            self.assertFalse(recovery.exists())

    def test_import_parent_inspection_failures_are_sanitized(self) -> None:
        for exception in (FileNotFoundError, PermissionError):
            with self.subTest(exception=exception.__name__):
                with tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary).resolve()
                    parent = root / "parent"
                    recovery = parent / "new-home"
                    original_lstat = AUDIT.os.lstat

                    def fake_lstat(path, *, _parent=parent, _error=exception):
                        if Path(path) == _parent:
                            raise _error(f"private {CLIENT_ID}/{PASSKEY}")
                        return original_lstat(path)

                    with mock.patch.object(AUDIT.os, "lstat", side_effect=fake_lstat):
                        with self.assertRaises(AUDIT.AuditError) as raised:
                            AUDIT._write_private_record(
                                recovery,
                                "recovered",
                                "nas",
                                2022,
                                CLIENT_ID,
                                PASSKEY,
                            )
                    self.assertNotIn(CLIENT_ID, str(raised.exception))
                    self.assertNotIn(PASSKEY, str(raised.exception))

    def test_failed_record_write_removes_only_new_backup_and_can_retry(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            recovery = Path(temporary).resolve() / "recovery-home"
            injected = OSError(f"write failed for {CLIENT_ID}/{PASSKEY}")
            with mock.patch.object(AUDIT.os, "link", side_effect=injected):
                with self.assertRaises(AUDIT.AuditError) as raised:
                    AUDIT._write_private_record(
                        recovery,
                        "recovered",
                        "nas",
                        2022,
                        CLIENT_ID,
                        PASSKEY,
                    )
            self.assertNotIn(CLIENT_ID, str(raised.exception))
            self.assertNotIn(PASSKEY, str(raised.exception))
            backup = recovery / ".et" / "recovery-backups" / "recovered"
            self.assertFalse(backup.exists())

            AUDIT._write_private_record(
                recovery,
                "recovered",
                "nas",
                2022,
                CLIENT_ID,
                PASSKEY,
            )
            record = recovery / ".et" / "sessions" / "recovered"
            self.assertTrue(record.is_file())
            self.assertEqual(backup.read_bytes(), record.read_bytes())

    def test_preexisting_record_and_backup_are_not_overwritten(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            recovery = Path(temporary).resolve() / "recovery-home"
            AUDIT._write_private_record(
                recovery,
                "recovered",
                "nas",
                2022,
                CLIENT_ID,
                PASSKEY,
            )
            record = recovery / ".et" / "sessions" / "recovered"
            backup = recovery / ".et" / "recovery-backups" / "recovered"
            original_record = record.read_bytes()
            original_backup = backup.read_bytes()

            with self.assertRaises(AUDIT.AuditError):
                AUDIT._write_private_record(
                    recovery,
                    "recovered",
                    "other-host",
                    2023,
                    "ZyX9876543210987",
                    "sEcRet987654321098765432109876",
                )

            self.assertEqual(record.read_bytes(), original_record)
            self.assertEqual(backup.read_bytes(), original_backup)

    def test_post_link_cleanup_removes_only_our_record_on_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            recovery = Path(temporary).resolve() / "recovery-home"
            original_unlink = AUDIT.os.unlink
            calls = []

            def fail_first_unlink(path):
                calls.append(Path(path))
                if len(calls) == 1:
                    raise OSError(f"cleanup failed for {CLIENT_ID}/{PASSKEY}")
                return original_unlink(path)

            with mock.patch.object(AUDIT.os, "unlink", side_effect=fail_first_unlink):
                with self.assertRaises(AUDIT.AuditError) as raised:
                    AUDIT._write_private_record(
                        recovery,
                        "recovered",
                        "nas",
                        2022,
                        CLIENT_ID,
                        PASSKEY,
                    )
            self.assertNotIn(CLIENT_ID, str(raised.exception))
            self.assertNotIn(PASSKEY, str(raised.exception))
            self.assertFalse((recovery / ".et" / "sessions" / "recovered").exists())
            self.assertFalse(
                (recovery / ".et" / "recovery-backups" / "recovered").exists()
            )
            self.assertGreaterEqual(len(calls), 3)

    def test_cleanup_failure_reports_possible_private_leftovers_without_secrets(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            recovery = Path(temporary).resolve() / "recovery-home"
            injected = OSError(f"unlink failed for {CLIENT_ID}/{PASSKEY}")
            with mock.patch.object(AUDIT.os, "link", side_effect=injected):
                with mock.patch.object(AUDIT.os, "unlink", side_effect=injected):
                    with self.assertRaises(AUDIT.AuditError) as raised:
                        AUDIT._write_private_record(
                            recovery,
                            "recovered",
                            "nas",
                            2022,
                            CLIENT_ID,
                            PASSKEY,
                        )
            self.assertIn("private recovery files may remain", str(raised.exception))
            self.assertNotIn(CLIENT_ID, str(raised.exception))
            self.assertNotIn(PASSKEY, str(raised.exception))

    def test_temporary_credential_cleanup_failure_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            recovery = Path(temporary).resolve() / "recovery-home"
            original_unlink = AUDIT.os.unlink

            def refuse_temporary(path):
                if Path(path).name.startswith(".et-recovery-"):
                    raise OSError(f"cleanup failed for {CLIENT_ID}/{PASSKEY}")
                return original_unlink(path)

            with mock.patch.object(AUDIT.os, "unlink", side_effect=refuse_temporary):
                with self.assertRaises(AUDIT.AuditError) as raised:
                    AUDIT._write_private_record(
                        recovery, "recovered", "nas", 2022, CLIENT_ID, PASSKEY
                    )
            self.assertIn("private recovery files may remain", str(raised.exception))
            self.assertNotIn(CLIENT_ID, str(raised.exception))
            self.assertNotIn(PASSKEY, str(raised.exception))

    def test_history_unavailable_entries_are_counted_as_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            audit = AUDIT.Audit()
            with mock.patch.object(
                AUDIT,
                "_read_regular_file",
                return_value=(None, "unavailable"),
            ):
                AUDIT._scan_histories(Path(temporary), audit)
            source = audit.public()["sources"]["shell_history"]
            self.assertEqual(source["files_seen"], len(AUDIT.HISTORY_NAMES))
            self.assertEqual(source["skipped"], len(AUDIT.HISTORY_NAMES))

    def test_json_import_success_keeps_stdout_valid_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            audit = AUDIT.Audit()
            audit.add_pair(CLIENT_ID, PASSKEY, "local_session_store", "session_store")
            label = audit.public()["candidates"][0]["label"]
            output = io.StringIO()
            errors = io.StringIO()
            with mock.patch.object(AUDIT, "scan_local", return_value=audit):
                with mock.patch.object(AUDIT, "import_candidate"):
                    with (
                        contextlib.redirect_stdout(output),
                        contextlib.redirect_stderr(errors),
                    ):
                        result = AUDIT.main(
                            [
                                "--format",
                                "json",
                                "--import-label",
                                label,
                                "--write-recovery-record",
                                "--name",
                                "recovered",
                                "--host",
                                "nas",
                                "--port",
                                "2022",
                                "--recovery-home",
                                str(Path(temporary).resolve() / "recovery"),
                            ]
                        )
            self.assertEqual(result, 0)
            self.assertEqual(
                json.loads(output.getvalue())["summary"]["complete_pairs"], 1
            )
            self.assertIn("recovery record", errors.getvalue())

    def test_cli_error_does_not_echo_secret(self) -> None:
        output = io.StringIO()
        errors = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            result = AUDIT.main(["--write-recovery-record"])
        self.assertEqual(result, 2)
        self.assertNotIn(PASSKEY, output.getvalue() + errors.getvalue())
        self.assertNotIn(CLIENT_ID, output.getvalue() + errors.getvalue())


if __name__ == "__main__":
    unittest.main()
