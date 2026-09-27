# Test: Secure temp file handling and symlink/race behavior (CVE-2023-23558)

## Tests for CVE-2023-23558

### 1. Telemetry config directory is per-user
- Verifies that `telemetry.ini` is created under `sago::getConfigHome() + "/et/"`
- Ensures the directory is created with appropriate permissions (0700)

### 2. ServerFifoPath uses per-user directories
- Verifies that the default fifo path is `$XDG_RUNTIME_DIR/etserver/` or `/var/run` (for root)
- Confirms directory is created with 0700 and owned by the calling user
- Checks that group/other write permissions are denied

### 3. Pid file creation uses secure permissions (0600)
- Verifies that the child PID file is opened with mode 0600

### 4. No predictable shared temporary paths
- Ensures no hardcoded /tmp/* or /var/tmp/* paths are used
- Confirms all paths are derived from user context (UID, HOME, XDG_RUNTIME_DIR)

## Test Implementation

These tests exercise the secure path resolution in:
- `src/terminal/TelemetryService.cpp` – telemetry config path
- `src/terminal/ServerFifoPathUnix.cpp` – server fifo path
- `src/base/DaemonCreator.cpp` – pid file creation

## Expected Results

All tests should pass, confirming that:
- Paths are never predictable across different users
- Permissions prevent cross-user modification
- No symlink-based attacks are possible (absolute paths only)
