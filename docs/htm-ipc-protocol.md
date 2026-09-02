# `htm` to `htmd` IPC Protocol

## Purpose

`htm` is the foreground client attached to a terminal emulator's PTY. `htmd`
is the persistent per-user daemon that owns multiplexer state and shell PTYs.
This document defines their local transport, framing, connection lifecycle, and
failure behavior.

For the terminal-emulator view of the same messages, including the layout JSON
and author responsibilities, see [HTM Terminal Emulator
Protocol](htm-terminal-protocol.md).

## Transport and endpoint

The peers communicate over a local stream socket through `PipeSocketHandler`.
There is no transport handshake, authentication exchange, encryption, or
version negotiation. Access control therefore depends on the operating system,
the per-user endpoint name, and local filesystem/process isolation. This
protocol MUST NOT be exposed as a network service.

The default endpoint is per user:

- Unix: `<temporary-directory>/htm.<numeric-uid>.ipc`, normally
  `/tmp/htm.<uid>.ipc`.
- Windows: `htm.<user>.ipc`, resolved relative to the user's temporary
  directory. A relative name is used because Windows `AF_UNIX` rejects
  drive-qualified paths.

`htm` attempts a connection up to five times, one second apart. The daemon
listens continuously but services only one active client. It does not accept a
waiting client until the active client disconnects; after acceptance it starts
recovery for the new client.

## Byte forwarding

Once connected, `htm` is a bidirectional bridge:

```text
terminal PTY input  -> htm stdin  -> IPC stream -> htmd
htmd IPC stream     -> htm stdout -> terminal PTY output
```

Messages are not acknowledged by `htm`, assigned sequence numbers, or
reordered. The bridge preserves their bytes and order. Unix `htm` additionally
recognizes complete server frame boundaries so it can apply bounded output
queues and backpressure without emitting partial frames; this does not change
the stream. Windows currently forwards available chunks directly.

## Framing

All ordinary messages use:

```text
type[1] + Base64(int32_le(payload_length))[8] + payload[payload_length]
```

The Base64 form is the standard alphabet with `=` padding. The length is a
signed 32-bit little-endian value and counts payload bytes as transmitted.
`SESSION_END` is the single ASCII byte `D`, without a length field.

The reader MUST accumulate partial fields and MUST allow multiple frames in a
single stream read. A negative or excessive length is invalid. Server input is
strict: an unexpected type or malformed field disconnects the client.

The message set is:

| Type | Name | Direction | Wire payload length | Wire payload |
| --- | --- | --- | --- | --- |
| `1` | `INSERT_KEYS` | `htm` -> `htmd` | `36 + Base64Length(n)` | pane UUID + Base64 input bytes |
| `2` | `INIT_STATE` | `htmd` -> `htm` | JSON byte count | raw UTF-8 JSON |
| `3` | `CLIENT_CLOSE_PANE` | `htm` -> `htmd` | 36 | pane UUID |
| `4` | `APPEND_TO_PANE` | `htmd` -> `htm` | `36 + Base64Length(n)` | pane UUID + Base64 PTY output |
| `5` | `NEW_TAB` | `htm` -> `htmd` | 72 | tab UUID + initial pane UUID |
| `8` | `SERVER_CLOSE_PANE` | `htmd` -> `htm` | 36 | pane UUID |
| `9` | `NEW_SPLIT` | `htm` -> `htmd` | 73 | source pane UUID + new pane UUID + ASCII `1` vertical or `0` horizontal |
| `A` | `RESIZE_PANE` | `htm` -> `htmd` | 52 | Base64 int32 columns + Base64 int32 rows + pane UUID |
| `B` | `DEBUG_LOG` | `htmd` -> `htm` | `Base64Length(n)` | Base64 diagnostic bytes |
| `C` | `INSERT_DEBUG_KEYS` | `htm` -> `htmd` | command byte count, at least 1 | raw diagnostic command bytes |
| `D` | `SESSION_END` | either direction | none | none |

Every UUID is exactly 36 ASCII bytes. `Base64Length(n)` is
`4 * ceil(n / 3)`. Empty Base64 fields have zero bytes.

The currently accepted client-to-daemon framed types are `1`, `3`, `5`, `9`,
`A`, and `C`. `D` is accepted as the standalone lifecycle byte. Sending a
server-to-client type in the other direction is a protocol error.

## Connection lifecycle

### Initial attachment and reconnect

After every accept, `htmd` writes the following in order:

1. raw six-byte terminal init marker `ESC [ # # # q`;
2. a `DEBUG_LOG` initialization message;
3. one `INIT_STATE` containing the complete current model;
4. zero or more `APPEND_TO_PANE` frames containing retained pane output;
5. a `DEBUG_LOG` ready message.

The init marker is deliberately outside normal framing. `htm` forwards it to
the terminal emulator. Recovery is a state snapshot plus bounded scrollback,
not a retransmission log: the protocol has no message IDs or acknowledgement
positions.

The daemon's multiplexer state and pane PTYs survive a client disconnect. A
replacement client receives another recovery sequence. The final pane closing
causes `htmd` to stop.

### Graceful close

`IpcPairEndpoint::closeEndpoint` writes `D` and then closes the stream. Receipt
of `D`, orderly EOF, or an unrecoverable read/write error ends the foreground
bridge. `htm` then emits the terminal-facing exit marker `ESC [ $ $ $ q` and
restores the console mode.

`D` is recognized only at a frame boundary. A `D` byte inside a declared
payload is data, not a close request.

### Daemon replacement

`htm -x` requests replacement of an existing per-user daemon before starting a
new one. Unix uses per-user process discovery/termination and removes a stale
socket path. Windows uses the named event
`Local\\EternalTerminal.HtmShutdown.<user>` for graceful shutdown, with process
termination as a compatibility fallback. The named event is lifecycle control,
not part of the stream protocol.

## Flow control and limits

The IPC protocol has no application-level flow-control messages. It relies on
stream-socket backpressure.

On Unix, `htm` bounds pending data in each direction to 256 KiB. It stops
reading the source whose destination queue is full, allowing kernel
backpressure to propagate. It validates server payload lengths against a 4 MiB
maximum before moving a complete frame to terminal output. Implementations
should use equivalent bounded queues and must continue servicing input while
terminal output is blocked.

The daemon encodes each currently available PTY-output chunk as its own
`APPEND_TO_PANE` frame. Consumers must not assume one frame corresponds to one
line, one escape sequence, or one application write.

## Compatibility constraints

The current framing encodes native C++ `int32_t` memory and the supported
implementations/tests define that as little-endian. All current supported
desktop targets are little-endian. A port to a big-endian host must still emit
and consume little-endian values to remain interoperable.

Because there is no negotiated protocol version:

- existing type values and payload layouts must remain stable;
- new framed message types can be added only when old receivers can safely
  skip them or after capability negotiation is introduced;
- framing, integer representation, and init-marker changes require a new
  negotiated protocol version.

## Failure handling

`htmd` disconnects the client when it receives an unknown client header,
invalid Base64, a short/closed stream, or another read/write failure. The daemon
continues running and keeps pane state unless it was explicitly stopped or has
no panes. A terminal integration should treat loss of `htm` as a detach and MAY
offer to launch a new `htm` process to recover.

Neither side currently sends structured error frames. Diagnostic details go to
process logs or `DEBUG_LOG`; callers must not depend on diagnostic text for
state transitions.
