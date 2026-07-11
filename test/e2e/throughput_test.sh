#!/bin/bash
# Measures bulk throughput through the full et pipeline on a fast link
# (no throttle proxy). Used to verify that flow control and kernel buffer
# tuning (TCP_NOTSENT_LOWAT, SO_SNDBUF clamps) do not regress throughput.
#
# Usage: bash throughput_test.sh [--flow-control <mode>] [--bytes N]
# Assumes the build/ directory contains current binaries.

FC_FLAG=""
BYTES=30000000
while [ $# -gt 0 ]; do
    case "$1" in
        --flow-control) FC_FLAG="--flow-control $2"; shift ;;
        --bytes) BYTES="$2"; shift ;;
    esac
    shift
done

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
BUILD="$REPO/build"
PORT=4446
ID="TPT$(date +%s | tail -c 8)"
KEY="E2ETestKey123456789012345678901A"
ETMUX="/tmp/et_tpt_client_$$.sock"
FIFO="/tmp/et_tpt_$$.fifo"

cleanup() {
    tmux -S "$ETMUX" kill-server 2>/dev/null
    kill $ETSERVER_PID 2>/dev/null
    kill -9 $(lsof -t -i:$PORT 2>/dev/null) 2>/dev/null
    pkill -9 -f "etterminal.*$ID" 2>/dev/null
    rm -f "$FIFO" "$ETMUX"
}
trap cleanup EXIT

"$BUILD/etserver" --serverfifo="$FIFO" --port=$PORT --logdir=/tmp &>/dev/null &
ETSERVER_PID=$!
sleep 2
"$BUILD/etterminal" --idpasskey="$ID/$KEY" --serverfifo="$FIFO" --logdir=/tmp &>/dev/null &
sleep 2

tmux -S "$ETMUX" new-session -d -s et -x 200 -y 50
for i in $(seq 1 30); do
    tmux -S "$ETMUX" capture-pane -p 2>/dev/null | grep -q '\$' && break
    sleep 1
done
tmux -S "$ETMUX" send-keys "$BUILD/et --idpasskey='$ID/$KEY' 127.0.0.1:$PORT $FC_FLAG" Enter
for i in $(seq 1 30); do
    tmux -S "$ETMUX" capture-pane -p 2>/dev/null | grep -qF '~]$' && break
    sleep 1
done

# The marker is split in the typed command so the echoed command line
# itself doesn't match the completion grep.
T0=$(python3 -c 'import time; print(f"{time.time():.3f}")')
tmux -S "$ETMUX" send-keys "base64 -w 200 /dev/zero | head -c $BYTES; echo 'TPT_''END_MARK'" Enter
ELAPSED="timeout"
for i in $(seq 1 1200); do
    sleep 0.1
    if tmux -S "$ETMUX" capture-pane -p 2>/dev/null | grep -qF 'TPT_END_MARK'; then
        ELAPSED=$(python3 -c "import time; print(f'{time.time() - $T0:.2f}')")
        break
    fi
done

if [ "$ELAPSED" = "timeout" ]; then
    echo "RESULT: timeout"
else
    MBPS=$(python3 -c "print(f'{$BYTES / float('$ELAPSED') / 1e6:.1f}')")
    echo "RESULT: ${BYTES} bytes in ${ELAPSED}s = ${MBPS} MB/s (flow-control: ${FC_FLAG:-default})"
fi
