# Session recovery after a client reboot

A surviving remote terminal still needs its original client ID and passkey
for reattachment. A PID, terminal title, or ID from a log filename is not
enough. Keep the surviving `etterminal` and `etserver` processes running
while investigating.

## Read-only investigation

Inspect local saved records, old client logs, and captured SSH startup output.
On the server, inspect terminal logs, process arguments and environment, and
open-file metadata. Read regular files only. Do not read PTYs, pipes, or
sockets. Reading these can consume another process's input.

Never paste raw matching lines, environment values, command lines, or session
files into an issue. They can contain credentials. Log filenames can contain
client IDs too. Report counts and opaque candidate labels instead.

Run the scanner from the repository:

```sh
python3 -B scripts/et-session-recovery.py --format json
python3 -B scripts/et-session-recovery.py --ssh nas --format json
```

The remote command sends the scanner source over SSH stdin. It does not
install a file on the server. Results contain counts and hashed labels.
Permission failures and bounded scans limit what can be concluded. A
candidate has not been authenticated. Historical log IDs are not a count of
live sessions.

Run the scanner's synthetic-data tests with:

```sh
python3 -B -m unittest discover -s scripts -p test_et_session_recovery.py
```

After building, run the local CLI persistence test with:

```sh
python3 -B test/system_tests/test_default_persistence.py
```

It uses the built client, server, and terminal with a private session store
and a loopback server. Its SSH substitute runs only the fixture's terminal
bootstrap locally. It does not connect to the NAS.

The SSH bootstrap generates an initial ID starting with `XXX`. Newer
terminals replace that initial pair and return the actual pair in
`IDPASSKEY:` output. A bootstrap command found in a verbose client log may
therefore contain a discarded credential. A match is a candidate, not proof
that authentication will succeed.

`IDPASSKEY:` uses the stdout-only logger. The normal `etterminal` log does
not receive that output. Captured SSH output may contain it.

## Registration socket boundary

Despite its name, `/var/run/etserver.idpasskey.fifo` is a Unix stream socket.
`UserTerminalRouter::acceptNewConnection` reads a `TERMINAL_USER_INFO` packet
and registers the terminal. This interface has no list or credential query.
Connecting and sending packets is not a read-only probe. A registration can
conflict with a live terminal or replace a dead registration.

`ServerConnection::clientKeys` and `UserTerminalRouter::idInfoMap` hold
credentials in memory. Surviving terminals retain their own credentials in
`UserTerminalHandler` and re-register after a router restart. Do not restart
the server to force this behavior during recovery.

If no complete credential pair survives in readable artifacts, the supported
protocol cannot recover an unnamed session. Process-memory extraction would
be a separate investigation. Debugger attachment, dumps, injected code, and
forced re-registration are outside this recovery procedure because they can
pause processes or expose unrelated secrets.

## Controlled recovery

A recovered pair must be placed in an isolated, owner-only session store.
Keep the original artifacts unchanged. Use a fresh temporary home directory
for the recovery client so normal records cannot be overwritten or removed.
Keep an owner-only backup of a candidate record before attaching: the client
removes a record when the server reports that the session has ended.

Confirm the target host and current ET port before creating a record. An old
saved port is not evidence of the current server configuration. Ask for
explicit confirmation before record creation and again before attaching.
An attach changes the live client connection and is not an authentication
probe with no side effects.

For a candidate found locally, record creation requires both
`--write-recovery-record` and `--import-label`, plus a session name, host,
port, and fresh `--recovery-home`. The tool asks for a candidate-specific
confirmation phrase. It does not attach automatically. Remote audit mode
does not transfer credentials or create local records.
The isolated home also contains an independent private record copy in
`.et/recovery-backups`, which survives deletion of the active record.

For a privileged audit, run the staged helper from a root shell and identify
the account whose home should be scanned:

```sh
python3 -B /home/eric/et-recovery-audit-20260906/et-session-recovery-root.py --user eric
```

It scans the same sources with root read access and writes a redacted report
to a fresh `/var/tmp/et-root-audit-*/report.json`. The directory is mode
`0700`, and the report is mode `0600`. Both belong to the selected account so
the report can be read over SSH. The helper prints the exact report path. It
does not create recovery records, read process memory, or alter ET processes.

## September 6, 2026 investigation

The local worktree was clean on `router-recovery`, commit `05e5db2df`.
Read-only checks at 22:17 UTC found 21 NAS `etterminal` processes. Their
start times ranged from September 4 through September 6. The running
`etserver` was PID 8395 and had started on August 24. The service was active.
The registration path was a Unix socket owned by root with mode `0777`.
The router constructor explicitly sets that mode.

The local session directory contained no records. The scanner found no
complete credential pairs in eight local client logs, two local server logs,
local shell history, or the inspected process arguments. A privileged local
scan included the two server logs that the first scan could not read.

The NAS scan also found no complete pairs. It read 230 terminal logs, two
client logs, and the 8,622,539-byte server log. It scanned two shell histories
and the most recent 200 journal entries for each service name (`et` and
`etserver`). It found 230 historical log IDs. Open-file metadata matched 21
of them to the surviving terminals. Arguments and accessible environments
were scanned for the 22 ET processes.

The root audit completed successfully. Its report is
`/var/tmp/et-root-audit-077o5ozz/report.json`, owned by `eric` with mode `0600`.
It inspected all 22 ET processes, including the server's environment and
open-file metadata. It reported no permission failures, skipped sources,
or warnings, and found zero complete credential pairs. Open-file metadata
still matched IDs for all 21 surviving terminals. A subsequent check
confirmed all 21 terminals and server PID 8395 were still running.

This closes the privileged metadata check. The search did not inspect
process memory, historical filesystem snapshots, or every possible custom
log location. No supported reattachment path was found in the audited
artifacts. Extracting credentials from live process memory would require a
separate, explicitly approved investigation.

TCP/2022 was listening, and the `et` service used
`/usr/bin/etserver --cfgfile=/etc/et.cfg --logtostdout`. No recovery record
was created and no attach was attempted. The observed evidence does not
provide a supported reattachment path. The surviving remote processes were
left running. The incident's stale `eofcase` record and failed attach are
user-supplied evidence; this investigation did not repeat that attach.

## Persistence validation

The updated client saves direct-session credentials before connection setup.
It assigns a generated name unless `--name` or `--no-persist` is supplied.
New records use atomic creation that refuses to replace an existing record.
The store synchronizes credential files and directory entries to disk.

On macOS, `bash format.sh` completed using Xcode's bundled formatter.
CMake was rerun, and `ninja` plus `ctest --parallel --output-on-failure`
passed all 202 entries. This includes the Python audit suite and the CLI
fixture that kills a client, restarts a loopback server, and reattaches using
the saved credentials. The CLI fixture also checks default naming, opt-out,
credential redaction, and record removal after the remote shell exits.

The built client is `build/et`. The installed client at
`/Users/eric/.bin/et` has not been replaced.
