#!/usr/bin/env python3
"""Run the read-only NAS audit as root and publish only its redacted report."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import pwd
import sys
import tempfile
from typing import Optional

STAGE = "initialization"


class AuditError(RuntimeError):
    """An expected, user-facing helper error without source details."""


class SafeArgumentParser(argparse.ArgumentParser):
    """Do not echo user supplied arguments in parse errors."""

    def error(self, _message: str) -> None:
        raise AuditError("invalid command-line options")


def safe_failure(error):
    frames = []
    traceback = error.__traceback__
    while traceback is not None:
        filename = Path(traceback.tb_frame.f_code.co_filename).name
        label = (
            filename
            if filename in {"et-session-recovery.py", "et-session-recovery-root.py"}
            else "library"
        )
        frames.append({"code": label, "line": traceback.tb_lineno})
        traceback = traceback.tb_next
    return {
        "status": "failed",
        "stage": STAGE,
        "exception_type": type(error).__name__,
        "frames": frames,
        "raw_diagnostics_recorded": False,
    }


def _build_parser() -> SafeArgumentParser:
    parser = SafeArgumentParser(
        prog="et-session-recovery-root",
        description="Run the read-only EternalTerminal session audit as root",
        add_help=True,
    )
    parser.add_argument(
        "--user",
        metavar="USER",
        help="account whose home directory should be scanned",
    )
    return parser


def _resolve_account(user: Optional[str]):
    explicit = user is not None
    if not explicit:
        user = os.environ.get("SUDO_USER", "")
        # Do not silently audit root's home when invoked from a root shell.
        if not user or user == "root":
            raise AuditError("target account is required; pass --user")
    if not user:
        raise AuditError("target account is required; pass --user")
    try:
        account = pwd.getpwnam(user)
    except (KeyError, OSError, TypeError, ValueError) as exc:
        raise AuditError("target account was not found") from exc
    if not explicit and account.pw_uid == 0:
        raise AuditError("target account is required; pass --user")
    home = getattr(account, "pw_dir", "")
    if not isinstance(home, str) or not home or not os.path.isabs(home):
        raise AuditError("target account has no usable home")
    return account


def _load_scanner():
    source = Path(__file__).with_name("et-session-recovery.py")
    spec = importlib.util.spec_from_file_location("et_recovery_audit", source)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load audit scanner")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _scan_account(account):
    global STAGE
    STAGE = "load scanner"
    module = _load_scanner()

    # Read only regular artifacts and process metadata, never process memory.
    STAGE = "scan sources"
    return module.scan_local(home=Path(account.pw_dir)).public()


def _write_report(report, account) -> None:
    global STAGE
    encoded = (json.dumps(report, sort_keys=True, indent=2) + "\n").encode()

    # A fresh directory avoids overwriting any previous report. Keep it private
    # throughout creation, then give the requested reader access to the report.
    STAGE = "write redacted report"
    directory = Path(tempfile.mkdtemp(prefix="et-root-audit-", dir="/var/tmp"))
    output = directory / "report.json"
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(encoded)
        stream.flush()
        os.fsync(stream.fileno())
        os.fchown(stream.fileno(), account.pw_uid, account.pw_gid)
        os.fchmod(stream.fileno(), 0o600)
    os.chown(directory, account.pw_uid, account.pw_gid)
    os.chmod(directory, 0o700)
    print(f"Redacted report: {output}")


def main(argv=None):
    global STAGE
    try:
        args = _build_parser().parse_args(argv)
        if os.geteuid() != 0:
            print("Run this script as root.", file=sys.stderr)
            return 2
        account = _resolve_account(args.user)
        try:
            report = _scan_account(account)
            report["status"] = "complete"
        except Exception as error:
            report = safe_failure(error)
        report["scope"] = "nas_root_read_only"
        report["effective_uid"] = os.geteuid()
        _write_report(report, account)
        return 0 if report["status"] == "complete" else 2
    except AuditError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    except Exception as error:
        # The exception text may contain source paths or credentials.
        print(json.dumps(safe_failure(error)), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.dont_write_bytecode = True
    try:
        raise SystemExit(main())
    except Exception as error:
        print(json.dumps(safe_failure(error)), file=sys.stderr)
        raise SystemExit(2)
