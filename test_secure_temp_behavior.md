# Secure temp file / symlink race tests (CVE-2023-23558)

## What is tested
- Telemetry config directory: must be created under per-user config home.
- Server fifo directory: must enforce 0700, same-owner, no group/other write.
- Pid file creation: must use 0600 permissions.

## Symlink / race behavior
- Directory creation uses `mkdir` + `stat` checks; no symlink-follow for directory target is performed, but the directory is user-controlled (`$XDG_RUNTIME_DIR` or `/var/run`).
- The code fails fast (`LOG(FATAL)`) if directory has wrong owner or group/other write bits, which prevents cross-user manipulation.
- For full symlink-race hardening, an `O_NOFOLLOW` approach or `fstat` after `open` could be added, but current behavior removes predictable shared paths.

## Residual risk
- Low: paths are per-user, not shared /tmp.
- Remaining hardening: add `fstat` verification after opening pid file; add `lstat` before connecting to fifo to reject symlinks.
