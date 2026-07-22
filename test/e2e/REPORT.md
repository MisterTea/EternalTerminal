# ET Flow Control: E2E Test Report

## Problem Statement (Issue #631)

When a process inside an ET session produces output faster than the network can
deliver it, ET has no mechanism to manage the mismatch. The result:

1. The terminal display falls progressively behind real time
2. Ctrl-C appears to do nothing: the interrupt is delivered, but tens of
   seconds of stale queued output must drain before the prompt reappears
3. In the worst case (laptop sleep, wifi drop), long-running jobs freeze
   entirely because the PTY kernel buffer fills up and `write()` blocks

## The fix, in two parts

**Part 1: application-level flow control.** Terminal output is staged in a
bounded `WriteBuffer` between the PTY and the client socket. Flow control
is strictly opt-in; users who don't opt in are completely unchanged:

- `--flow-control none` (default): the exact pre-feature code path. Output
  is written straight to the connection with blocking writes; no
  application buffering, no kernel socket tuning.
- `--flow-control backpressure`: when the buffer fills, stop reading from
  the PTY. The remote process pauses, exactly like plain ssh. Lossless —
  required for correctness-sensitive consumers like `tmux -CC`.
- `--flow-control discard`: when the buffer fills, drop the *oldest* pending
  output. The remote process never stalls and the display stays close to
  real time. Old output is lost from scrollback while the link is saturated.

**Part 2: kernel buffer tuning (opt-in sessions only).** Flow control only
helps if pending data actually waits in the application buffer. An audit of
every buffer on the path found kernel-side reservoirs that were absorbing
the backlog downstream of any point where ET could drop or gate it. These
are tuned per-session, only when the client opted in (the server learns the
mode from InitialPayload; etterminal learns it via TermInit):

| Buffer | Untuned size | Fix |
|---|---|---|
| TCP send buffer (server->client) | autotunes to multi-MB (20MB max on this host) | `TCP_NOTSENT_LOWAT=32KB`: select() only reports writable when <32KB is unsent, so the backlog stays in the WriteBuffer. In-flight (sent-but-unacked) data is not limited, preserving high-BDP throughput. |
| ET WriteBuffer | 256KB | 64KB — only needs to absorb bursts between drain opportunities |
| etterminal->etserver unix socket | ~200KB (net.core.wmem_default) | `SO_SNDBUF=64KB` on the sender side |
| PTY kernel buffer | ~64KB | not tunable from userspace; part of the fixed lag floor |
| BackedWriter backup (64MB) | n/a | reconnect recovery only; never delays live data |

## Test Setup

```
print_timestamps.py -> PTY -> etterminal -> etserver --[TCP]--> throttle_proxy --[TCP]--> et client -> terminal
                                                       100KB/s
```

All components run on a single machine via `--idpasskey` (no SSH). A userspace
TCP throttle proxy between etserver and the et client limits server-to-client
throughput to 100KB/s. The proxy clamps `SO_RCVBUF` on its server-facing
socket to 64KB so it models a real slow link instead of silently absorbing
megabytes in its own autotuned receive buffer.

**Workload**: `print_timestamps.py` prints 1000 timestamped lines in a burst
every 10ms (~2.7MB/s raw output — 27x the proxy's capacity).

**Metrics** (sampled every 5s; Ctrl-C sent after the workload):

- **display_lag**: wall clock minus the newest timestamp visible on the
  client's terminal — how stale the screen is.
- **process_lag**: wall clock minus the timestamp the producer most recently
  wrote (via a sidecar file that bypasses ET). Large values mean the process
  is blocked in `write()`.
- **ctrl_c_latency**: time from sending Ctrl-C until the shell prompt is
  visible again. This is the original issue 631 complaint.

## Results (2026-07-10, devvm, Linux 6.13; re-validated on the v7.0.0 base)

Upstream v7.0.0 independently added disconnect buffering (#731, #762: the
connection buffers up to 64MB while disconnected so processes don't freeze
immediately) and a PTY-input deadlock fix (#765). Those address the
disconnect freeze, but not the connected saturated link: with a live but
slow connection, writes still land in the autotuned kernel send buffer, so
the `none` baseline below reproduces issue #631 unchanged on v7.0.0.

### Slow link, 30s saturation

| scenario | display lag @30s | process | Ctrl-C -> prompt |
|---|---|---|---|
| `none` (default — status quo, v7.0.0 base) | 32.3s, growing ~1s/s | throttled to link rate | **115.8s** |
| backpressure, no kernel tuning (pre-v7 base) | 31.5s, growing | throttled | 53.8s |
| discard, no kernel tuning (pre-v7 base) | 24.4s, growing | full speed | 28.0s |
| **backpressure + tuning (opt-in)** | **~2s, bounded** | throttled (lossless) | **2.7s** |
| **discard + tuning (opt-in)** | **~1.1s, bounded** | **full speed** | **2.1s** |

The `none` row is the default build with no flag: unbounded kernel queue,
growing lag, and Ctrl-C takes however long the queue takes to drain (high
variance run to run; 56-116s observed across bases). This is deliberate:
current users see zero change unless they opt in.

The no-kernel-tuning rows show why the tuning is essential: with the default
autotuned TCP send buffer, megabytes of stale output pile up in the kernel
where the WriteBuffer can neither gate nor drop them, and both modes are
barely better than trunk. With `TCP_NOTSENT_LOWAT`, display lag stops growing
entirely: it is bounded by the small fixed pipeline (WriteBuffer + unsent
bytes + link queue) instead of scaling with how long the link has been
saturated.

### 60s disconnect at t=30s (tuned)

| scenario | process during disconnect | after reconnect | Ctrl-C |
|---|---|---|---|
| discard | **full speed, never stalls** | fresh output in <5s, no stale replay | 1.6-2.1s |
| backpressure | stalls (lossless contract, like ssh; the v7 default instead buffers up to 64MB before stalling) | resumes, display recovers in <5s | 2.6s |

etserver stayed alive through all scenarios.

### Bulk throughput (localhost, no throttle, 30MB through the full pipeline)

Measurements are noisy on a shared devvm (trunk build: 40-47 MB/s across
runs; this branch: 20-41 MB/s across modes, with `none`, `backpressure`,
and `discard` overlapping run-to-run). Two things hold: all modes stay in
the tens of MB/s, far beyond any realistic terminal workload, and the
default `none` path is the identical pre-feature code, so non-opted-in
users cannot regress. On real WAN links throughput is BDP-bound, which
`TCP_NOTSENT_LOWAT` does not restrict.

## Why the default is `none`

The requirement is that current users are no worse off — the strongest
form of that is byte-identical behavior, so the default (CLI flag and
proto field alike, which also covers old clients talking to new servers)
is the legacy path with stock kernel buffering.

`backpressure` is the conservative opt-in: it keeps today's lossless
semantics (a slow client pauses the producer, exactly like plain ssh; safe
for `tmux -CC`) while bounding the queue so Ctrl-C takes ~3s instead of
minutes. `discard` is the opt-in for freshness over completeness — the
`cat /dev/zero | base64` case from the issue, ML training jobs, builds:
the process never stalls (even fully disconnected) and the display tracks
real time. The cost is that saturated-link output is missing from
scrollback.

## What this means for real-world use

- **ML training / long build, laptop closed**: with `none` or
  `backpressure`, the job pauses when buffers fill — same as today. With
  `--flow-control discard`, the job keeps running at full speed and you see
  current output when you reconnect.
- **`cat` a huge file over a slow link, then Ctrl-C**: prompt returns in
  ~2-3s in either opt-in mode (default/today: a minute or more).
- **tmux -CC**: use `none` (default) or `backpressure`. Every byte is
  delivered.

## Reproducing These Results

```bash
cd /path/to/EternalTerminal/build
cmake -DDISABLE_VCPKG=ON -GNinja .. && ninja -j4

# One scenario (trunk|none|backpressure|discard) [--disconnect]
bash test/e2e/do_scenario.sh discard --outdir /tmp/et_e2e
bash test/e2e/do_scenario.sh discard --disconnect --outdir /tmp/et_e2e

# Bulk throughput on a fast link
bash test/e2e/throughput_test.sh
bash test/e2e/throughput_test.sh --flow-control discard
```

See `test/e2e/README.md` for manual setup and `test/e2e/throttle_proxy.py` for
details on the proxy implementation.
