# `htm` to `htmd` IPC Protocol

## Purpose

`htm` is the foreground client attached to a terminal emulator's PTY. `htmd`
is the persistent per-user daemon that owns multiplexer state and shell PTYs.
This document defines their local transport, byte stream, connection
lifecycle, and failure behavior.

The bytes on this socket **are** tmux control mode: commands from `htm` toward
`htmd`, notifications and `%begin`/`%end` blocks the other way. There is no
second framing layer. For command and notification meaning, see [HTM and tmux
Control Mode](htm-terminal-protocol.md) and the
[tmux Control Mode wiki](https://github.com/tmux/tmux/wiki/Control-Mode).

## Transport and endpoint

The peers communicate over a local stream socket through `PipeSocketHandler`.
There is no transport handshake, authentication exchange, encryption, or
version negotiation. Access control therefore depends on the operating system,
the per-user endpoint name, and local filesystem/process isolation. This
socket MUST NOT be exposed as a network service.

The default endpoint is per user:

- Unix: `<temporary-directory>/htm.<numeric-uid>.ipc`, normally
  `/tmp/htm.<uid>.ipc`.
- Windows: `htm.<user>.ipc`, resolved relative to the user's temporary
  directory. A relative name is used because Windows `AF_UNIX` rejects
  drive-qualified paths.

`htm` attempts a connection up to five times, one second apart. The daemon
listens continuously but services only one active client. Accepting a new
client disconnects the previous one and runs recovery for the newcomer.

## Byte forwarding

Once connected, `htm` is a bidirectional byte bridge:

```text
terminal PTY input  -> htm stdin  -> IPC stream -> htmd
htmd IPC stream     -> htm stdout -> terminal PTY output
```

`htm` does not parse, acknowledge, sequence, or reorder the stream. Unix
`htm` applies bounded nonblocking queues in each direction so a stalled
terminal read cannot stop it from accepting input. Windows currently forwards
available chunks directly.

Two sequences are **not** part of the IPC stream. `htm` writes them only to
the terminal PTY:

- DCS `ESC P 1000 p` immediately before it starts forwarding, so the emulator
  enters tmux `-CC` mode;
- ST `ESC \` when `htm` exits, so the emulator leaves that mode.

`htmd` never emits DCS or ST.

## Line discipline on the daemon

`htmd` reads the IPC stream as control-mode text. It splits commands on CR,
LF, or CRLF. iTerm2's tmux gateway writes CR; a real tmux client usually sees
LF because the PTY has ICRNL. `htm` puts stdin in raw mode, so the daemon
accepts both.

An empty line detaches the client. Non-empty lines are tmux command lists
(unquoted `;` separates commands). `htmd` logs each command and replies with
`%begin`/`%end` or `%begin`/`%error`. Asynchronous notifications (`%output`,
`%layout-change`, …) are queued while a reply block is open and flushed
after `%end`/`%error`, matching tmux.

## Connection lifecycle

### Initial attachment and reconnect

After every accept, `htmd` writes:

1. an empty server-originated reply (`%begin <t> 1 0` / `%end <t> 1 0`);
2. snapshot notifications for the attached session (`%sessions-changed`,
   `%session-changed`, `%window-add`, `%layout-change`,
   `%session-window-changed`, `%window-pane-changed`).

Recovery is that snapshot, not a retransmission log: the protocol has no
message IDs or acknowledgement positions. Pane output that arrived while no
client was attached is not replayed as `%output`.

The daemon's multiplexer state and pane PTYs survive a client disconnect. A
replacement client receives another recovery sequence. Closing the last pane,
or `kill-server`, stops `htmd`.

### Graceful close

Either side closing the stream ends the foreground bridge. `htmd` also
detaches when it receives an empty line, `detach-client`, or `exit`; it then
emits `%exit` and closes the socket. `htm` writes ST to the terminal and
restores console mode.

There is no application-level close byte on the socket.

### Daemon replacement

`htm -x` requests replacement of an existing per-user daemon before starting
a new one. Unix uses per-user process discovery/termination and removes a
stale socket path. Windows uses the named event
`Local\\EternalTerminal.HtmShutdown.<user>` for graceful shutdown, with
process termination as a compatibility fallback. The named event is lifecycle
control, not part of the stream protocol.

## Flow control and limits

The IPC path has no extra application-level flow-control messages beyond what
tmux control mode already defines (`refresh-client -f pause-after`,
`refresh-client -A`, `%pause` / `%continue` / `%extended-output`). Socket
backpressure still applies.

On Unix, `htm` bounds pending data in each direction to 256 KiB. It stops
reading the source whose destination queue is full, allowing kernel
backpressure to propagate. Implementations should use equivalent bounded
queues and must continue servicing input while terminal output is blocked.

Each currently available PTY-output chunk becomes its own `%output` (or
`%extended-output`) line. Consumers must not assume one notification is one
line, one escape sequence, or one application write.

## Compatibility constraints

Because the wire format is tmux control mode:

- command names, flags, notification shapes, and layout strings SHOULD stay
  compatible with the GUI clients HTM targets (iTerm2, WezTerm, Hyper
  plugins, and others that speak `tmux -CC`);
- `#{version}` is reported as `3.5a` so those clients enable modern flags;
- unknown commands already return `%error`; new commands can be added without
  a private version handshake;
- `%layout-change` uses tmux 3.x's four-field form (layout, visible layout,
  flags), including an empty flags field with a trailing space.

## Failure handling

`htmd` disconnects the client on a short/closed stream or an unrecoverable
read/write error. Parse failures are reported as `%error` and do not by
themselves stop the daemon. The daemon keeps pane state unless it was
explicitly stopped or has no panes. A terminal integration should treat loss
of `htm` as a detach and MAY offer to launch a new `htm` process to recover.

Neither side sends a private error frame type. Diagnostic details go to
process logs (`control command: …` on the daemon). Callers must not depend on
log text for state transitions.
