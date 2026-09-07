#!/usr/bin/env python3
"""Read-only EternalTerminal session credential audit.

The default operation only reads bounded, explicitly named sources and emits
metadata.  It never opens a router socket/FIFO, attaches to a process, or
contacts an ET server.  The optional import operation writes a new private
session record under a caller supplied recovery HOME after an interactive
confirmation.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Callable, Optional


SCHEMA_VERSION = 1
MAX_SOURCE_BYTES = 16 * 1024 * 1024
MAX_SESSION_BYTES = 128 * 1024
MAX_PROCESS_BYTES = 512 * 1024
MAX_COMMAND_BYTES = 2 * 1024 * 1024
SERVICE_NAMES = ("et", "etserver")

ID_PATTERN = r"[A-Za-z0-9]{16}"
PASSKEY_PATTERN = r"[A-Za-z0-9]{32}"
PAIR_RE = re.compile(
    rf"(?<![A-Za-z0-9])(?P<id>{ID_PATTERN})/(?P<passkey>{PASSKEY_PATTERN})(?![A-Za-z0-9])"
)
FILENAME_ID_RE = re.compile(rf"(?<![A-Za-z0-9])(?P<id>{ID_PATTERN})(?![A-Za-z0-9])")
SESSION_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$")
LABEL_RE = re.compile(r"^candidate-[0-9a-f]{16}$")

LOG_NAME_RE = re.compile(
    r"^(?:et(?:terminal|server|client)|etjump)(?:[-_.].*)?$", re.IGNORECASE
)
HISTORY_NAMES = (
    ".zsh_history",
    ".bash_history",
    ".sh_history",
    ".ksh_history",
    ".local/share/fish/fish_history",
    ".config/fish/fish_history",
)

ROUTER_PATHS = (
    ("root_router", "/var/run/etserver.idpasskey.fifo"),
    ("xdg_router", "etserver.idpasskey.fifo"),
    ("home_router", ".local/share/etserver/etserver.idpasskey.fifo"),
)


class AuditError(RuntimeError):
    """An expected, user-facing audit error without source details."""


class SafeArgumentParser(argparse.ArgumentParser):
    """Do not echo user supplied arguments in parse errors."""

    def error(self, _message: str) -> None:
        raise AuditError("invalid command-line options")


def _hash_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()[:16]


def _pair_label(client_id: str, passkey: str) -> str:
    return "candidate-" + _hash_bytes(
        client_id.encode("ascii") + b"\0" + passkey.encode("ascii")
    )


def _id_label(client_id: str) -> str:
    return "id-" + _hash_bytes(client_id.encode("ascii"))


def _safe_text(value: bytes) -> str:
    return value.decode("utf-8", errors="replace")


def _is_regular_mode(mode: int) -> bool:
    return stat.S_ISREG(mode) and not stat.S_ISLNK(mode)


def _read_regular_file(path: Path, limit: int) -> tuple[Optional[bytes], str]:
    """Read one bounded regular file, never following its final symlink.

    The status is intentionally a small category.  It is safe to expose and
    does not include a path or an operating-system error string.
    """

    try:
        before = os.lstat(path)
    except PermissionError:
        return None, "permission_denied"
    except OSError:
        return None, "unavailable"
    parent_ok, parent_reason = _safe_directory(path.parent)
    if not parent_ok:
        return None, parent_reason
    if stat.S_ISLNK(before.st_mode):
        return None, "symlink_skipped"
    if not stat.S_ISREG(before.st_mode):
        return None, "nonregular_skipped"
    if before.st_size > limit:
        return None, "oversize_skipped"

    flags = os.O_RDONLY
    nofollow = getattr(os, "O_NOFOLLOW", 0)
    flags |= nofollow
    fd = -1
    try:
        fd = os.open(path, flags | getattr(os, "O_NONBLOCK", 0))
        after = os.fstat(fd)
        if not _is_regular_mode(after.st_mode):
            return None, "nonregular_skipped"
        if after.st_dev != before.st_dev or after.st_ino != before.st_ino:
            return None, "changed_skipped"
        if after.st_size > limit:
            return None, "oversize_skipped"
        chunks: list[bytes] = []
        remaining = limit + 1
        while remaining > 0:
            chunk = os.read(fd, min(1024 * 1024, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        data = b"".join(chunks)
        if len(data) > limit:
            return None, "oversize_skipped"
        return data, "read"
    except PermissionError:
        return None, "permission_denied"
    except OSError:
        return None, "unavailable"
    finally:
        if fd >= 0:
            try:
                os.close(fd)
            except OSError:
                pass


def _safe_directory(path: Path) -> tuple[bool, str]:
    current = Path(path.anchor) if path.is_absolute() else Path(".")
    parts = path.parts[1:] if path.is_absolute() else path.parts
    for part in parts:
        current /= part
        try:
            info = os.lstat(current)
        except FileNotFoundError:
            return False, "unavailable"
        except PermissionError:
            return False, "permission_denied"
        except OSError:
            return False, "unavailable"
        if stat.S_ISLNK(info.st_mode):
            return False, "symlink_skipped"
        if not stat.S_ISDIR(info.st_mode):
            return False, "non_directory"
    return True, "read"


def _iter_regular_children(path: Path) -> tuple[list[Path], dict[str, int]]:
    stats = {
        "entries_seen": 0,
        "symlinks_skipped": 0,
        "nonregular_skipped": 0,
        "unavailable": 0,
        "permission_denied": 0,
    }
    ok, reason = _safe_directory(path)
    if not ok:
        stats[reason if reason in stats else "unavailable"] += 1
        return [], stats
    children: list[Path] = []
    try:
        with os.scandir(path) as entries:
            for entry in entries:
                stats["entries_seen"] += 1
                try:
                    info = entry.stat(follow_symlinks=False)
                except PermissionError:
                    stats["permission_denied"] += 1
                    continue
                except OSError:
                    stats["unavailable"] += 1
                    continue
                if stat.S_ISLNK(info.st_mode):
                    stats["symlinks_skipped"] += 1
                    continue
                if not stat.S_ISREG(info.st_mode):
                    stats["nonregular_skipped"] += 1
                    continue
                children.append(Path(entry.path))
    except PermissionError:
        stats["permission_denied"] += 1
    except OSError:
        stats["unavailable"] += 1
    return children, stats


class Candidate:
    def __init__(self, client_id: str, passkey: str) -> None:
        self.client_id = client_id
        self.passkey = passkey
        self.label = _pair_label(client_id, passkey)
        self.source_kinds: set[str] = set()
        self.evidence: set[str] = set()
        self.occurrences = 0
        self.id_only_matches = 0
        self.endpoints: set[tuple[str, int]] = set()

    @property
    def bootstrap_unverified(self) -> bool:
        return self.client_id.startswith("XXX")

    def add(
        self,
        source_kind: str,
        evidence: str,
        endpoint: Optional[tuple[str, int]] = None,
    ) -> None:
        self.source_kinds.add(source_kind)
        self.evidence.add(evidence)
        self.occurrences += 1
        if endpoint is not None:
            self.endpoints.add(endpoint)

    def public(self) -> dict[str, Any]:
        confidence = "low" if self.bootstrap_unverified else "high"
        if not self.bootstrap_unverified and "session_store" not in self.evidence:
            confidence = "medium"
        return {
            "label": self.label,
            "sources": sorted(self.source_kinds),
            "occurrences": self.occurrences,
            "complete": True,
            "bootstrap_unverified": self.bootstrap_unverified,
            "confidence": confidence,
            "auth_probe": "not_attempted",
            "eligible_for_import": not self.bootstrap_unverified,
            "matched_id_only_evidence": self.id_only_matches,
            "endpoint_count": len(self.endpoints),
        }


class Audit:
    def __init__(self, scope: str = "local") -> None:
        self.scope = scope
        self.candidates: dict[tuple[str, str], Candidate] = {}
        self.id_only: dict[str, set[str]] = {}
        self.sources: dict[str, dict[str, int]] = {}
        self.service_checks: list[dict[str, Any]] = []
        self.router_paths: list[dict[str, Any]] = []
        self.warnings: set[str] = set()

    def source(self, kind: str) -> dict[str, int]:
        return self.sources.setdefault(
            kind,
            {
                "files_seen": 0,
                "files_read": 0,
                "bytes_read": 0,
                "pairs_found": 0,
                "ids_found": 0,
                "skipped": 0,
                "errors": 0,
                "permission_denied": 0,
            },
        )

    def add_pair(
        self,
        client_id: str,
        passkey: str,
        source_kind: str,
        evidence: str = "combined",
        endpoint: Optional[tuple[str, int]] = None,
    ) -> None:
        key = (client_id, passkey)
        candidate = self.candidates.setdefault(key, Candidate(client_id, passkey))
        candidate.add(source_kind, evidence, endpoint)
        self.source(source_kind)["pairs_found"] += 1
        for _ in self.id_only.get(client_id, set()):
            candidate.id_only_matches += 1

    def add_id_only(self, client_id: str, source_kind: str) -> None:
        self.id_only.setdefault(client_id, set()).add(source_kind)
        self.source(source_kind)["ids_found"] += 1
        for candidate in self.candidates.values():
            if candidate.client_id == client_id:
                candidate.id_only_matches += 1

    def ingest_text(
        self,
        text: str,
        source_kind: str,
        evidence: str = "combined",
        endpoint: Optional[tuple[str, int]] = None,
    ) -> None:
        for match in PAIR_RE.finditer(text):
            self.add_pair(
                match.group("id"),
                match.group("passkey"),
                source_kind,
                evidence,
                endpoint,
            )

    def public(self) -> dict[str, Any]:
        complete = list(self.candidates.values())
        importable = [item for item in complete if not item.bootstrap_unverified]
        id_only = [
            {
                "label": _id_label(client_id),
                "sources": sorted(kinds),
                "complete": False,
                "matched_complete_pair": any(
                    candidate.client_id == client_id for candidate in complete
                ),
            }
            for client_id, kinds in sorted(self.id_only.items())
            if not any(candidate.client_id == client_id for candidate in complete)
        ]
        return {
            "schema": SCHEMA_VERSION,
            "mode": "audit",
            "scope": self.scope,
            "read_only": True,
            "server_authentication": "not_attempted",
            "mutations": [],
            "remote_actions": [],
            "sources": {
                kind: dict(sorted(stats.items()))
                for kind, stats in sorted(self.sources.items())
            },
            "router_paths": self.router_paths,
            "service_checks": self.service_checks,
            "candidates": [
                candidate.public()
                for candidate in sorted(complete, key=lambda item: item.label)
            ],
            "id_only": sorted(id_only, key=lambda item: item["label"]),
            "summary": {
                "complete_pairs": len(complete),
                "non_bootstrap_pairs": len(importable),
                "bootstrap_unverified_pairs": len(complete) - len(importable),
                "id_only_without_pair": len(id_only),
            },
            "warnings": sorted(self.warnings),
        }

    def find_candidate(self, label: str) -> Optional[Candidate]:
        if not LABEL_RE.fullmatch(label):
            return None
        return next(
            (
                candidate
                for candidate in self.candidates.values()
                if candidate.label == label
            ),
            None,
        )


def _parse_session_record(
    data: bytes,
) -> Optional[tuple[str, str, str, int, str]]:
    fields: dict[str, str] = {}
    try:
        for line in _safe_text(data).splitlines():
            if not line or "=" not in line:
                continue
            key, value = line.split("=", 1)
            if key in {
                "version",
                "name",
                "host",
                "port",
                "id",
                "passkey",
                "savedat",
                "title",
            }:
                fields[key] = value
        if fields.get("version") != "1":
            return None
        client_id = fields.get("id", "")
        passkey = fields.get("passkey", "")
        name = fields.get("name", "")
        host = fields.get("host", "")
        port = int(fields.get("port", "0"))
        int(fields.get("savedat", ""))
        if (
            not SESSION_NAME_RE.fullmatch(name)
            or not re.fullmatch(ID_PATTERN, client_id)
            or not re.fullmatch(PASSKEY_PATTERN, passkey)
            or not host
            or "\n" in host
            or not (1 <= port <= 65535)
        ):
            return None
        return client_id, passkey, name, port, host
    except (TypeError, ValueError):
        return None


def _scan_session_store(home: Path, audit: Audit) -> None:
    kind = "local_session_store"
    source = audit.source(kind)
    files, directory_stats = _iter_regular_children(home / ".et" / "sessions")
    source["files_seen"] += directory_stats["entries_seen"]
    source["skipped"] += (
        directory_stats["symlinks_skipped"]
        + directory_stats["nonregular_skipped"]
        + directory_stats["unavailable"]
    )
    source["permission_denied"] += directory_stats["permission_denied"]
    for path in files:
        data, status = _read_regular_file(path, MAX_SESSION_BYTES)
        if data is None:
            if status == "permission_denied":
                source["permission_denied"] += 1
            else:
                source["skipped"] += 1
            continue
        source["files_read"] += 1
        source["bytes_read"] += len(data)
        parsed = _parse_session_record(data)
        if parsed is None:
            continue
        client_id, passkey, name, port, host = parsed
        audit.add_pair(
            client_id,
            passkey,
            kind,
            "session_store",
            (host, port),
        )


def _log_kind(name: str) -> str:
    lower = name.lower()
    if "terminal" in lower or lower.startswith("etjump"):
        return "etterminal_log"
    if "server" in lower:
        return "etserver_log"
    return "etclient_log"


def _scan_log_directories(home: Path, audit: Audit) -> None:
    directories: list[Path] = []
    directory_keys: set[tuple[int, int]] = set()
    values = [
        "/tmp",
        "/private/tmp",
        os.environ.get("TMPDIR", ""),
        str(home / "Library/Logs"),
    ]
    for value in values:
        if not value or not value.startswith("/"):
            continue
        path = Path(value)
        try:
            info = os.lstat(path)
        except OSError:
            continue
        if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
            continue
        path = Path(os.path.realpath(path))
        key = (info.st_dev, info.st_ino)
        if key not in directory_keys:
            directory_keys.add(key)
            directories.append(path)
    for directory in directories:
        directory_ok, _ = _safe_directory(directory)
        if not directory_ok:
            audit.warnings.add("log_directory_unavailable")
            continue
        try:
            entries = list(os.scandir(directory))
        except OSError:
            audit.warnings.add("log_directory_unavailable")
            continue
        for entry in entries:
            if not LOG_NAME_RE.fullmatch(entry.name):
                continue
            kind = _log_kind(entry.name)
            source = audit.source(kind)
            source["files_seen"] += 1
            path = Path(entry.path)
            data, status = _read_regular_file(path, MAX_SOURCE_BYTES)
            if data is None:
                if status == "permission_denied":
                    source["permission_denied"] += 1
                else:
                    source["skipped"] += 1
                continue
            source["files_read"] += 1
            source["bytes_read"] += len(data)
            text = _safe_text(data)
            audit.ingest_text(text, kind, "idpasskey_output")
            if kind == "etterminal_log":
                for match in FILENAME_ID_RE.finditer(entry.name):
                    audit.add_id_only(match.group("id"), "etterminal_filename")


def _scan_histories(home: Path, audit: Audit) -> None:
    kind = "shell_history"
    source = audit.source(kind)
    for relative in HISTORY_NAMES:
        path = home / relative
        source["files_seen"] += 1
        data, status = _read_regular_file(path, MAX_SOURCE_BYTES)
        if data is None:
            if status == "permission_denied":
                source["permission_denied"] += 1
            else:
                source["skipped"] += 1
            continue
        source["files_read"] += 1
        source["bytes_read"] += len(data)
        audit.ingest_text(_safe_text(data), kind, "history_command")


def _run_capture(argv: list[str], timeout: float = 4.0) -> tuple[Optional[bytes], str]:
    try:
        result = subprocess.run(
            argv,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None, "unavailable"
    if result.returncode != 0:
        return result.stdout[:MAX_COMMAND_BYTES], "failed"
    return result.stdout[:MAX_COMMAND_BYTES], "read"


def _proc_pid_directories() -> list[Path]:
    proc = Path("/proc")
    ok, _ = _safe_directory(proc)
    if not ok:
        return []
    result: list[Path] = []
    try:
        with os.scandir(proc) as entries:
            for entry in entries:
                if not entry.name.isdigit():
                    continue
                try:
                    info = entry.stat(follow_symlinks=False)
                except OSError:
                    continue
                if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode):
                    continue
                result.append(Path(entry.path))
    except OSError:
        return []
    return result


def _scan_proc_fds(pid_dir: Path, audit: Audit, targeted: bool) -> None:
    if not targeted:
        return
    kind = "process_fd_metadata"
    source = audit.source(kind)
    fd_dir = pid_dir / "fd"
    ok, reason = _safe_directory(fd_dir)
    if not ok:
        if reason == "permission_denied":
            source["permission_denied"] += 1
        return
    try:
        with os.scandir(fd_dir) as entries:
            for entry in entries:
                source["files_seen"] += 1
                try:
                    # fd entries are symlinks.  readlink is metadata only;
                    # never open the target, and never print its name.
                    target = os.readlink(entry.path)
                except PermissionError:
                    source["permission_denied"] += 1
                    continue
                except OSError:
                    source["skipped"] += 1
                    continue
                source["files_read"] += 1
                for match in FILENAME_ID_RE.finditer(target):
                    audit.add_id_only(match.group("id"), kind)
    except PermissionError:
        source["permission_denied"] += 1
    except OSError:
        source["errors"] += 1


def _scan_processes(audit: Audit) -> None:
    kind = "process_state"
    source = audit.source(kind)
    proc_available, proc_reason = _safe_directory(Path("/proc"))
    proc_dirs = _proc_pid_directories()
    if proc_available:
        if not proc_dirs:
            audit.warnings.add("process_state_empty_or_unavailable")
        for pid_dir in proc_dirs:
            source["processes_seen"] = source.get("processes_seen", 0) + 1
            cmd_data, cmd_status = _read_regular_file(
                pid_dir / "cmdline", MAX_PROCESS_BYTES
            )
            if cmd_data is None:
                if cmd_status == "permission_denied":
                    source["permission_denied"] += 1
                else:
                    source["skipped"] += 1
                continue
            source["files_read"] += 1
            source["bytes_read"] += len(cmd_data)
            cmd_parts = [_safe_text(part) for part in cmd_data.split(b"\0") if part]
            cmdline = " ".join(cmd_parts)
            executable = Path(cmd_parts[0]).name.lower() if cmd_parts else ""
            targeted = executable in {"et", "etterminal", "etserver"} or any(
                part == "--idpasskey"
                or part.startswith("--idpasskey=")
                or part == "--idpasskeyfile"
                or part.startswith("--idpasskeyfile=")
                for part in cmd_parts[1:]
            )
            if targeted:
                source["targeted_processes"] = source.get("targeted_processes", 0) + 1
                audit.ingest_text(cmdline, kind, "process_cmdline")
                env_data, env_status = _read_regular_file(
                    pid_dir / "environ", MAX_PROCESS_BYTES
                )
                if env_data is not None:
                    source["files_read"] += 1
                    source["bytes_read"] += len(env_data)
                    audit.ingest_text(
                        _safe_text(env_data.replace(b"\0", b" ")),
                        kind,
                        "process_environment",
                    )
                elif env_status == "permission_denied":
                    source["permission_denied"] += 1

                idpasskey_files: list[str] = []
                for index, part in enumerate(cmd_parts):
                    if part == "--idpasskeyfile" and index + 1 < len(cmd_parts):
                        idpasskey_files.append(cmd_parts[index + 1])
                    elif part.startswith("--idpasskeyfile="):
                        idpasskey_files.append(part.split("=", 1)[1])
                for filename in idpasskey_files:
                    if not filename.startswith("/"):
                        continue
                    file_data, file_status = _read_regular_file(
                        Path(filename), MAX_SESSION_BYTES
                    )
                    if file_data is not None:
                        file_source = audit.source("process_idpasskeyfile")
                        file_source["files_seen"] += 1
                        file_source["files_read"] += 1
                        file_source["bytes_read"] += len(file_data)
                        audit.ingest_text(
                            _safe_text(file_data),
                            "process_idpasskeyfile",
                            "idpasskey_file",
                        )
                    elif file_status == "permission_denied":
                        audit.source("process_idpasskeyfile")["permission_denied"] += 1
                _scan_proc_fds(pid_dir, audit, True)
        if source.get("permission_denied", 0):
            audit.warnings.add("process_state_partial_permission_denied")
        return

    if proc_reason == "permission_denied":
        audit.warnings.add("process_state_permission_denied")

    # macOS does not expose /proc.  ps output is captured and scanned in
    # memory; no command line or environment text is returned to the caller.
    output, _ = _run_capture(["ps", "-axo", "pid=,command="])
    if output is None:
        audit.warnings.add("process_state_unavailable")
        return
    source["files_read"] += 1
    source["bytes_read"] += len(output)
    for line in _safe_text(output).splitlines():
        if "etterminal" not in line.lower() and "etserver" not in line.lower():
            continue
        audit.ingest_text(line, kind, "process_command")


def _safe_state(value: str) -> str:
    if value in {"active", "inactive", "failed", "running", "dead", "exited"}:
        return value
    return "unknown"


def _scan_services(audit: Audit) -> None:
    for service in SERVICE_NAMES:
        show, show_status = _run_capture(
            [
                "systemctl",
                "show",
                "--no-pager",
                "--property=ActiveState,SubState,MainPID,ExecMainStartTimestamp",
                service,
            ]
        )
        check: dict[str, Any] = {
            "service": service,
            "manager": "systemd",
            "status": (
                "available"
                if show is not None and show_status == "read"
                else "failed"
                if show is not None
                else "unavailable"
            ),
            "active_state": "unknown",
            "sub_state": "unknown",
            "journal": "unavailable",
        }
        if show is not None:
            values: dict[str, str] = {}
            for line in _safe_text(show).splitlines():
                if "=" in line:
                    key, value = line.split("=", 1)
                    if key in {"ActiveState", "SubState"}:
                        values[key] = value
            check["active_state"] = _safe_state(values.get("ActiveState", ""))
            check["sub_state"] = _safe_state(values.get("SubState", ""))
            audit.ingest_text(
                _safe_text(show), f"{service}_service_state", "service_state"
            )
        journal, journal_status = _run_capture(
            ["journalctl", "--no-pager", "--quiet", "-u", service, "-n", "200"]
        )
        if journal is not None:
            check["journal"] = "available" if journal_status == "read" else "failed"
            journal_source = audit.source(f"{service}_journal")
            journal_source["files_read"] += 1
            journal_source["bytes_read"] += len(journal)
            audit.ingest_text(
                _safe_text(journal), f"{service}_journal", "journal_entry"
            )
        audit.service_checks.append(check)


def _router_kind(mode: int) -> str:
    if stat.S_ISSOCK(mode):
        return "unix_socket"
    if stat.S_ISFIFO(mode):
        return "fifo"
    if stat.S_ISREG(mode):
        return "regular_file"
    if stat.S_ISDIR(mode):
        return "directory"
    if stat.S_ISLNK(mode):
        return "symlink"
    return "other"


def _scan_router_metadata(home: Path, audit: Audit) -> None:
    xdg = os.environ.get("XDG_RUNTIME_DIR", "")
    paths = [
        (ROUTER_PATHS[0][0], Path(ROUTER_PATHS[0][1])),
        (
            ROUTER_PATHS[1][0],
            Path(xdg) / "etserver" / ROUTER_PATHS[1][1]
            if xdg.startswith("/")
            else home / ".local/share/etserver/etserver.idpasskey.fifo",
        ),
        (ROUTER_PATHS[2][0], home / ROUTER_PATHS[2][1]),
    ]
    seen: set[Path] = set()
    for category, path in paths:
        if path in seen:
            continue
        seen.add(path)
        try:
            info = os.lstat(path)
        except OSError:
            audit.router_paths.append(
                {"category": category, "present": False, "payload_read": False}
            )
            continue
        kind = _router_kind(info.st_mode)
        audit.router_paths.append(
            {
                "category": category,
                "present": True,
                "kind": kind,
                "mode_owner_bits": oct(stat.S_IMODE(info.st_mode) & 0o700),
                "world_or_group_writable": bool(stat.S_IMODE(info.st_mode) & 0o077),
                "payload_read": False,
            }
        )


def scan_local(
    home: Optional[Path] = None,
    include_processes: bool = True,
    include_services: bool = True,
) -> Audit:
    audit = Audit("local")
    raw_home = Path(home or os.environ.get("HOME", str(Path.home())))
    try:
        raw_home = Path(os.path.abspath(os.path.expanduser(str(raw_home))))
        home_info = os.lstat(raw_home)
    except OSError:
        audit.warnings.add("home_unavailable")
        return audit
    if stat.S_ISLNK(home_info.st_mode) or not stat.S_ISDIR(home_info.st_mode):
        audit.warnings.add("home_symlink_or_non_directory")
        return audit
    # Canonicalize system aliases such as macOS /var -> /private/var after
    # rejecting a caller supplied symlink as the HOME itself.
    target_home = Path(os.path.realpath(raw_home))
    _scan_session_store(target_home, audit)
    _scan_log_directories(target_home, audit)
    _scan_histories(target_home, audit)
    if include_processes:
        _scan_processes(audit)
    if include_services:
        _scan_services(audit)
    _scan_router_metadata(target_home, audit)
    return audit


SAFE_SSH_OPTION_RE = re.compile(
    r"^-o(?:BatchMode|ConnectTimeout|IdentitiesOnly|IdentityFile|"
    r"StrictHostKeyChecking|UserKnownHostsFile|PreferredAuthentications)=[^\r\n]+$"
)


def _validate_ssh_options(options: list[str]) -> None:
    for option in options:
        if not SAFE_SSH_OPTION_RE.fullmatch(option):
            raise AuditError("unsupported SSH option")


def _report_has_sensitive_fields(value: Any) -> bool:
    # Remote JSON must contain only this report's vocabulary. Reject unknown
    # keys and free-form strings, including split/renamed credential fields.
    keys = set(
        """schema mode scope read_only server_authentication mutations
        remote_actions sources router_paths service_checks candidates id_only
        summary warnings files_seen files_read bytes_read pairs_found ids_found
        skipped errors permission_denied processes_seen targeted_processes label
        occurrences complete bootstrap_unverified confidence auth_probe
        eligible_for_import matched_id_only_evidence endpoint_count
        matched_complete_pair category present kind mode_owner_bits
        world_or_group_writable payload_read service manager status active_state
        sub_state journal complete_pairs non_bootstrap_pairs
        bootstrap_unverified_pairs id_only_without_pair""".split()
    )
    source_names = set(
        """local_session_store etclient_log etserver_log
        etterminal_log etterminal_filename shell_history process_state
        process_fd_metadata process_idpasskeyfile et_service_state
        etserver_service_state et_journal etserver_journal""".split()
    )
    words = source_names | set(
        """audit local remote not_attempted
        ssh_python_source_stdin root_router xdg_router home_router unix_socket
        fifo regular_file directory symlink other et etserver systemd available
        unavailable unknown active inactive failed running dead exited read
        low medium high log_directory_unavailable process_state_unavailable
        process_state_partial_permission_denied process_state_permission_denied
        process_state_empty_or_unavailable home_unavailable
        home_symlink_or_non_directory""".split()
    )
    if isinstance(value, dict):
        if any(key not in keys | source_names for key in value):
            return True
        return any(_report_has_sensitive_fields(item) for item in value.values())
    if isinstance(value, list):
        return any(_report_has_sensitive_fields(item) for item in value)
    if isinstance(value, str):
        return not (
            value in words
            or re.fullmatch(r"(?:candidate|id)-[0-9a-f]{16}|0o[0-7]00|0o0", value)
        )
    return type(value) not in {int, bool}


def _remote_audit(ssh_host: str, ssh_options: list[str]) -> dict[str, Any]:
    if not ssh_host or ssh_host.startswith("-"):
        raise AuditError("invalid remote host")
    _validate_ssh_options(ssh_options)
    command = [
        "ssh",
        *ssh_options,
        "-oBatchMode=yes",
        "-oConnectTimeout=10",
        ssh_host,
        "python3",
        "-",
        "--remote-child",
    ]
    try:
        script = Path(__file__).read_bytes()
    except OSError as exc:
        raise AuditError("could not read audit script") from exc
    try:
        result = subprocess.run(
            command,
            input=script,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise AuditError("remote audit unavailable") from exc
    if result.returncode != 0:
        raise AuditError("remote audit failed")
    try:
        value = json.loads(result.stdout.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise AuditError("remote audit returned invalid result") from exc
    if (
        not isinstance(value, dict)
        or value.get("schema") != SCHEMA_VERSION
        or _report_has_sensitive_fields(value)
    ):
        raise AuditError("remote audit returned invalid result")
    value["scope"] = "remote"
    value["read_only"] = True
    value["mutations"] = []
    value["remote_actions"] = ["ssh_python_source_stdin"]
    return value


def _validate_session_name(name: str) -> None:
    if not SESSION_NAME_RE.fullmatch(name):
        raise AuditError("invalid recovery session name")


def _validate_endpoint(host: str, port: int) -> None:
    if not host or "\r" in host or "\n" in host:
        raise AuditError("invalid recovery endpoint")
    if port < 1 or port > 65535:
        raise AuditError("invalid recovery endpoint")


def _absolute_path(value: str) -> Path:
    if not value:
        raise AuditError("recovery HOME is required")
    raw_path = Path(os.path.abspath(os.path.expanduser(value)))
    if not raw_path.is_absolute() or raw_path == Path("/"):
        raise AuditError("invalid recovery HOME")
    # A caller supplied final symlink is rejected.  Canonicalize established
    # system aliases such as macOS /var -> /private/var for the later checks.
    try:
        final_info = os.lstat(raw_path)
    except FileNotFoundError:
        final_info = None
    except OSError as exc:
        raise AuditError("invalid recovery HOME") from exc
    if final_info is not None and stat.S_ISLNK(final_info.st_mode):
        raise AuditError("recovery HOME contains symlink")
    path = Path(os.path.realpath(raw_path))
    real_home = Path(os.path.realpath(os.environ.get("HOME", str(Path.home()))))
    if path == real_home:
        raise AuditError("recovery HOME must be isolated")
    try:
        existing_et = os.lstat(path / ".et")
    except FileNotFoundError:
        existing_et = None
    except OSError as exc:
        raise AuditError("invalid recovery HOME") from exc
    if existing_et is not None:
        raise AuditError("recovery HOME already contains ET state")
    return path


def _ensure_private_directory(path: Path, create: bool) -> bool:
    created = False
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        if not create:
            raise AuditError("recovery HOME is missing")
        try:
            path.mkdir(mode=0o700)
        except OSError as exc:
            raise AuditError("could not create private recovery directory") from exc
        try:
            info = os.lstat(path)
        except OSError as exc:
            raise AuditError("could not inspect recovery directory") from exc
        created = True
    except OSError as exc:
        raise AuditError("could not inspect recovery directory") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        raise AuditError("recovery path is not a directory")
    if hasattr(os, "geteuid") and info.st_uid != os.geteuid():
        raise AuditError("recovery directory is not owner controlled")
    if stat.S_IMODE(info.st_mode) & 0o077:
        raise AuditError("recovery directory is not private")
    return created


def _write_private_record(
    recovery_home: Path,
    name: str,
    host: str,
    port: int,
    client_id: str,
    passkey: str,
) -> None:
    # Reject ancestors that another account could rename while we create the
    # store. Root-owned sticky temporary directories are safe parent locations.
    for ancestor in reversed(recovery_home.parents):
        try:
            info = os.lstat(ancestor)
        except OSError as exc:
            raise AuditError("could not inspect recovery parent directory") from exc
        trusted_owner = info.st_uid in {0, os.geteuid()}
        sticky_root = info.st_uid == 0 and bool(info.st_mode & stat.S_ISVTX)
        if (
            not stat.S_ISDIR(info.st_mode)
            or not trusted_owner
            or (info.st_mode & 0o022 and not sticky_root)
        ):
            raise AuditError("recovery HOME has an unsafe parent directory")

    created_directories: list[tuple[Path, tuple[int, int]]] = []
    target = recovery_home / ".et" / "sessions" / name
    backup_path = recovery_home / ".et" / "recovery-backups" / name
    backup_created = False
    backup_identity: Optional[tuple[int, int]] = None
    record_created = False
    record_identity: Optional[tuple[int, int]] = None
    committed = False
    temporary_cleanup_failed = False

    def remember_directory(path: Path) -> None:
        try:
            info = os.lstat(path)
        except OSError:
            return
        created_directories.append((path, (info.st_dev, info.st_ino)))

    def cleanup_created_state() -> bool:
        nonlocal backup_created, record_created
        cleanup_failed = temporary_cleanup_failed
        if record_created:
            if record_identity is None:
                cleanup_failed = True
            else:
                try:
                    info = os.lstat(target)
                    if (info.st_dev, info.st_ino) == record_identity:
                        os.unlink(target)
                except FileNotFoundError:
                    pass
                except OSError:
                    cleanup_failed = True
        if backup_created:
            if backup_identity is None:
                cleanup_failed = True
            else:
                try:
                    info = os.lstat(backup_path)
                    if (info.st_dev, info.st_ino) == backup_identity:
                        os.unlink(backup_path)
                except FileNotFoundError:
                    pass
                except OSError:
                    cleanup_failed = True
            backup_created = False
        # Remove only empty directories created by this invocation. This
        # leaves any pre-existing record, backup, or unrelated user data alone.
        for path, identity in reversed(created_directories):
            try:
                info = os.lstat(path)
                if (info.st_dev, info.st_ino) != identity or not stat.S_ISDIR(
                    info.st_mode
                ):
                    continue
                os.rmdir(path)
            except OSError:
                pass
        return cleanup_failed

    try:
        try:
            os.mkdir(recovery_home, 0o700)
        except OSError as exc:
            raise AuditError("recovery HOME must be a new private directory") from exc
        else:
            remember_directory(recovery_home)
            _ensure_private_directory(recovery_home, False)

        et_dir = recovery_home / ".et"
        sessions_dir = et_dir / "sessions"
        if _ensure_private_directory(et_dir, True):
            remember_directory(et_dir)
        if _ensure_private_directory(sessions_dir, True):
            remember_directory(sessions_dir)

        try:
            target_info = os.lstat(target)
        except FileNotFoundError:
            target_info = None
        except OSError as exc:
            raise AuditError("could not inspect recovery record") from exc
        if target_info is not None:
            raise AuditError("recovery record already exists")

        contents = (
            "version=1\n"
            f"name={name}\n"
            f"host={host}\n"
            f"port={port}\n"
            f"id={client_id}\n"
            f"passkey={passkey}\n"
            f"savedat={int(time.time())}\n"
            "title=\n"
        ).encode("utf-8")
        # An attach may remove a stale active record. Keep an independent copy.
        backup_dir = et_dir / "recovery-backups"
        if _ensure_private_directory(backup_dir, True):
            remember_directory(backup_dir)
        backup_fd = -1
        try:
            backup_fd = os.open(
                backup_path,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
                0o600,
            )
            backup_created = True
            backup_info = os.fstat(backup_fd)
            backup_identity = (backup_info.st_dev, backup_info.st_ino)
            with os.fdopen(backup_fd, "wb") as backup:
                backup_fd = -1
                os.fchmod(backup.fileno(), 0o600)
                backup.write(contents)
                backup.flush()
                os.fsync(backup.fileno())
        except FileExistsError as exc:
            raise AuditError("recovery backup already exists") from exc
        except Exception as exc:
            raise AuditError("could not write private recovery backup") from exc
        finally:
            if backup_fd >= 0:
                try:
                    os.close(backup_fd)
                except OSError:
                    pass

        temporary: Optional[Path] = None
        fd = -1
        try:
            fd, temporary_name = tempfile.mkstemp(
                prefix=".et-recovery-", dir=str(sessions_dir)
            )
            temporary = Path(temporary_name)
            os.fchmod(fd, 0o600)
            written = 0
            while written < len(contents):
                count = os.write(fd, contents[written:])
                if count <= 0:
                    raise OSError("record write failed")
                written += count
            os.fsync(fd)
            os.close(fd)
            fd = -1
            # Hard-link the complete file into place, so an existing target cannot
            # be replaced by a race. The source record is never changed.
            os.link(temporary, target, follow_symlinks=False)
            record_created = True
            try:
                record_info = os.lstat(target)
                record_identity = (record_info.st_dev, record_info.st_ino)
            except OSError as exc:
                raise AuditError("could not inspect private recovery record") from exc
            os.unlink(temporary)
            temporary = None
            try:
                directory_fd = os.open(sessions_dir, os.O_RDONLY)
                try:
                    os.fsync(directory_fd)
                finally:
                    os.close(directory_fd)
            except OSError:
                # The record is complete even on platforms that do not fsync dirs.
                pass
        except FileExistsError as exc:
            raise AuditError("recovery record already exists") from exc
        except Exception as exc:
            raise AuditError("could not write private recovery record") from exc
        finally:
            if fd >= 0:
                try:
                    os.close(fd)
                except OSError:
                    pass
            if temporary is not None:
                try:
                    os.unlink(temporary)
                except OSError:
                    temporary_cleanup_failed = True
        committed = True
    except AuditError:
        raise
    except Exception as exc:
        raise AuditError("could not write private recovery record") from exc
    finally:
        if not committed:
            if cleanup_created_state():
                raise AuditError(
                    "private recovery files may remain after cleanup failure"
                )


def import_candidate(
    audit: Audit,
    label: str,
    name: str,
    host: str,
    port: int,
    recovery_home: str,
    confirm: Callable[[str], str] = input,
    allow_unverified_bootstrap: bool = False,
) -> None:
    candidate = audit.find_candidate(label)
    if candidate is None:
        raise AuditError("candidate was not found")
    if candidate.bootstrap_unverified and not allow_unverified_bootstrap:
        raise AuditError("candidate is unverified bootstrap material")
    _validate_session_name(name)
    _validate_endpoint(host, port)
    path = _absolute_path(recovery_home)
    phrase = f"IMPORT {label}"
    try:
        response = confirm(f"Type {phrase} to confirm writing a recovery record: ")
    except (EOFError, KeyboardInterrupt) as exc:
        raise AuditError("interactive confirmation required") from exc
    if response.strip() != phrase:
        raise AuditError("interactive confirmation did not match")
    _write_private_record(
        path,
        name,
        host,
        port,
        candidate.client_id,
        candidate.passkey,
    )


def _build_parser() -> SafeArgumentParser:
    parser = SafeArgumentParser(
        prog="et-session-recovery",
        description="Read-only EternalTerminal session recovery audit",
        add_help=True,
    )
    parser.add_argument("--ssh", metavar="HOST")
    parser.add_argument("--ssh-option", action="append", default=[])
    parser.add_argument("--home", metavar="PATH")
    parser.add_argument("--format", choices=("text", "json"), default="text")
    parser.add_argument("--no-processes", action="store_true")
    parser.add_argument("--no-services", action="store_true")
    parser.add_argument("--import-label", metavar="LABEL")
    parser.add_argument("--write-recovery-record", action="store_true")
    parser.add_argument("--allow-unverified-bootstrap", action="store_true")
    parser.add_argument("--name", metavar="NAME")
    parser.add_argument("--host", metavar="HOST")
    parser.add_argument("--port", type=int, metavar="PORT")
    parser.add_argument("--recovery-home", metavar="PATH")
    parser.add_argument("--remote-child", action="store_true", help=argparse.SUPPRESS)
    return parser


def _render_text(report: dict[str, Any]) -> str:
    lines = [
        "EternalTerminal session recovery audit",
        f"scope: {report.get('scope', 'unknown')}",
        "read_only: yes",
        "server_authentication: not_attempted",
    ]
    summary = report.get("summary", {})
    lines.append(
        "complete_pairs: "
        f"{summary.get('complete_pairs', 0)} "
        f"(non_bootstrap={summary.get('non_bootstrap_pairs', 0)}, "
        f"bootstrap_unverified={summary.get('bootstrap_unverified_pairs', 0)})"
    )
    lines.append(f"id_only_without_pair: {summary.get('id_only_without_pair', 0)}")
    for kind, stats in report.get("sources", {}).items():
        lines.append(
            f"source {kind}: files={stats.get('files_seen', 0)} "
            f"read={stats.get('files_read', 0)} "
            f"pairs={stats.get('pairs_found', 0)} "
            f"ids={stats.get('ids_found', 0)} "
            f"skipped={stats.get('skipped', 0)} "
            f"denied={stats.get('permission_denied', 0)}"
        )
    for candidate in report.get("candidates", []):
        lines.append(
            f"candidate {candidate['label']}: complete=yes "
            f"importable={'yes' if candidate['eligible_for_import'] else 'no'} "
            f"confidence={candidate['confidence']} "
            f"sources={','.join(candidate['sources'])}"
        )
    for candidate in report.get("id_only", []):
        lines.append(
            f"id-only {candidate['label']}: sources={','.join(candidate['sources'])}"
        )
    for check in report.get("service_checks", []):
        lines.append(
            f"service {check.get('service', 'unknown')}: "
            f"status={check.get('status', 'unknown')} "
            f"active={check.get('active_state', 'unknown')} "
            f"sub={check.get('sub_state', 'unknown')} "
            f"journal={check.get('journal', 'unknown')}"
        )
    for router in report.get("router_paths", []):
        lines.append(
            f"router {router['category']}: "
            f"present={'yes' if router['present'] else 'no'} "
            f"kind={router.get('kind', 'absent')} payload_read=no"
        )
    if report.get("warnings"):
        lines.append("warnings: " + ",".join(report["warnings"]))
    if report.get("mutations"):
        lines.append("mutations: " + ",".join(report["mutations"]))
    return "\n".join(lines) + "\n"


def _emit(report: dict[str, Any], output_format: str) -> None:
    if output_format == "json":
        print(json.dumps(report, sort_keys=True, separators=(",", ":")))
    else:
        print(_render_text(report), end="")


def main(argv: Optional[list[str]] = None) -> int:
    parser = _build_parser()
    try:
        args = parser.parse_args(argv)
        if args.remote_child:
            if args.ssh or args.import_label or args.write_recovery_record:
                raise AuditError("invalid remote invocation")
            report = scan_local(
                include_processes=not args.no_processes,
                include_services=not args.no_services,
            ).public()
            _emit(report, "json")
            return 0
        if args.ssh and args.home:
            raise AuditError("remote audit cannot use local HOME override")
        if args.ssh and (
            args.import_label
            or args.write_recovery_record
            or args.name
            or args.host
            or args.port
            or args.recovery_home
        ):
            raise AuditError("remote audit cannot write recovery records")
        if args.write_recovery_record != bool(args.import_label):
            raise AuditError(
                "import requires --import-label and --write-recovery-record"
            )
        if args.allow_unverified_bootstrap and not args.import_label:
            raise AuditError("bootstrap override requires an import")
        if args.import_label and not all(
            value is not None
            for value in (args.name, args.host, args.port, args.recovery_home)
        ):
            raise AuditError("import requires name, host, port, and recovery HOME")
        if not args.import_label and any(
            value is not None
            for value in (args.name, args.host, args.port, args.recovery_home)
        ):
            raise AuditError("recovery record fields require an import")

        if args.ssh:
            report = _remote_audit(args.ssh, args.ssh_option)
            _emit(report, args.format)
            return 0

        audit = scan_local(
            Path(args.home) if args.home else None,
            include_processes=not args.no_processes,
            include_services=not args.no_services,
        )
        report = audit.public()
        _emit(report, args.format)
        if args.import_label:
            import_candidate(
                audit,
                args.import_label,
                args.name,
                args.host,
                args.port,
                args.recovery_home,
                allow_unverified_bootstrap=args.allow_unverified_bootstrap,
            )
            # Report success without printing a target path or any credential.
            print(
                f"recovery record and private backup created for {args.import_label}",
                file=sys.stderr if args.format == "json" else sys.stdout,
            )
        return 0
    except AuditError as exc:
        # Keep the error vocabulary intentionally generic; source paths and
        # exception strings can contain IDs, passkeys, or user command text.
        print(f"error: {exc}", file=sys.stderr)
        return 2
    except Exception:
        print("error: audit failed", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
