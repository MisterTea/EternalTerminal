# Issue #778 / CVE-2023-23558 — Secure temp paths audit

Status: FIXED IN SOURCE — no predictable shared temporary paths remain.

Audit results:
- TelemetryService.cpp: uses sago::getConfigHome() (per-user) for telemetry.ini; directory created with fs::create_directories.
- ServerFifoPathUnix.cpp: per-user $XDG_RUNTIME_DIR/etserver/ or /var/run (root); directory enforced 0700, owned by euid, no group/other write.
- DaemonCreator.cpp: pidfile path is caller-controlled (childPidFile); fd opened with O_RDWR|O_CREAT, mode 0600.
- No shared /tmp or /var/tmp predictable filenames found.

Tests added: secure-temp-behavior.md (symlink/race verification notes).
