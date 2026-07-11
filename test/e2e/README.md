# End-to-End Flow Control Testing

This directory contains tools for testing ET's flow control behavior with a
simulated slow network.

## Overview

The test uses a TCP throttle proxy between the ET client and server to simulate
a bandwidth-limited network. The proxy applies TCP backpressure to the server
(reads slowly), which forces the server's flow control to kick in.

```
print_timestamps.py -> PTY -> etterminal -> etserver -> [throttle proxy] -> et client -> terminal
                                                          (100KB/s)
```

The `--idpasskey` flag on both `et` and `etterminal` bypasses SSH, so you can
run all three processes on the same machine without SSH access.

## Prerequisites

Build ET first:

```bash
cd build && cmake .. && ninja
```

## Quick Start

Run the test script (must run outside process supervisors that kill children,
e.g. via cron or a dedicated terminal):

```bash
# From the repo root:
bash test/e2e/run_e2e_test.sh discard 100000
```

The script starts etserver (port 4444), a throttle proxy (port 4445 -> 4444 at
100KB/s), etterminal, and the et client in a tmux session. It then runs
`print_timestamps.py` and measures:

- **display_lag**: how far behind the timestamps on screen are from real time
- **process_lag**: how far behind the process producing output is from real time
  (0 = running freely, >1 = stalled by backpressure)

## Manual Setup

If you prefer to run each component in a separate terminal:

```bash
# Generate credentials (id must be 16 chars, key must be 32 chars)
ID="TestID0123456789"
KEY="TestPasskey0123456789012345678AB"

# Terminal 1: etserver
./build/etserver --serverfifo=/tmp/test_et.fifo --port=4444

# Terminal 2: throttle proxy (100KB/s)
python3 test/e2e/throttle_proxy.py 4445 4444 100000

# Terminal 3: etterminal
./build/etterminal --idpasskey="$ID/$KEY" --serverfifo=/tmp/test_et.fifo

# Terminal 4: et client
./build/et --idpasskey="$ID/$KEY" 127.0.0.1:4445 --flow-control discard

# Inside the ET session, run:
python3 test/e2e/print_timestamps.py
```

Then compare the timestamps on screen with `date` to measure lag.

## What to Expect

### Without flow control (baseline, before this PR)

The server reads from the PTY and writes directly to the TCP socket. When the
proxy is slow, `writePacket()` blocks, which blocks PTY reads, which stalls the
process. Data already in the TCP kernel buffer is "old" and gets delivered to the
client over time.

- **display_lag**: grows linearly (~1s per second)
- **process_lag**: grows (process stalls)
- **Ctrl-C**: may be slow because old data must drain first

### With `--flow-control backpressure`

Same as baseline: when the 256KB WriteBuffer fills and the socket is not
writable, the server stops reading from the PTY. The process stalls.

- **display_lag**: grows (bounded by TCP buffer + WriteBuffer = ~400KB)
- **process_lag**: grows (process stalls when buffer fills)
- **Ctrl-C**: responsive because buffered data is bounded

### With `--flow-control discard`

The WriteBuffer always accepts data and discards oldest chunks when full. The
server keeps reading from the PTY even when the socket is slow. The process
never stalls. Old data is discarded, so when the socket becomes writable, the
server sends *recent* data.

- **display_lag**: grows (bounded by TCP buffer, smaller than backpressure)
- **process_lag**: near zero (process runs freely)
- **Ctrl-C**: responsive

## Throttle Proxy Details

`throttle_proxy.py` is a TCP proxy that rate-limits the server→client direction
by reading from the server socket at a controlled rate. This causes the kernel
TCP send buffer on the server side to fill up, which makes the server's
`writePacket()` calls block — exactly what happens on a slow network.

The client→server direction (keystrokes, Ctrl-C) is forwarded at full speed.

The proxy binds on `[::]` with `IPV6_V6ONLY=0` (dual-stack) to accept both IPv4
and IPv6-mapped IPv4 connections, which is required because ET's
`TcpSocketHandler::connect()` may use either address family depending on
`getaddrinfo()` results.

## Running via Cron (for automated environments)

If your environment kills background processes (e.g. Claude Code), schedule the
test via cron:

```bash
echo "* * * * * bash /path/to/test/e2e/run_e2e_test.sh discard 100000 > /tmp/e2e_result.log 2>&1" | crontab -
# Wait ~2 minutes, then:
cat /tmp/e2e_result.log
crontab -r
```

## Troubleshooting

- **"proxy saw no connection"**: ET connected to a different port (likely the
  system etserver on port 2022). Make sure `--idpasskey` is being used so SSH
  config parsing is skipped.
- **etserver crashes with EINVAL**: Fixed by handling EBADF/EINVAL in
  `waitOnSocketWritable()` (see Headers.hpp). The fd becomes invalid when the
  client disconnects mid-drain.
- **"Connection refused" on proxy**: The proxy process died. Check if port 4445
  is already in use (`lsof -i:4445`).
