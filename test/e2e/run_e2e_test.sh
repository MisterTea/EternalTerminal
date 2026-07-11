#!/bin/bash
# End-to-end flow control test. Runs etserver, a throttle proxy, etterminal,
# and et client in tmux, then measures timestamp lag.
#
# Usage: bash run_e2e_test.sh [discard|backpressure] [rate_bytes_per_sec]
#
# Must be run outside of any process supervisor that kills children
# (e.g. via cron). See test/e2e/README.md for details.

MODE="${1:-discard}"
RATE="${2:-100000}"
DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD="$(cd "$DIR/../../build" && pwd)"
SOCK="/tmp/et_e2e_$$.sock"
FIFO="/tmp/et_e2e_$$.fifo"
ID="E2E$(date +%s)"
KEY="E2ETestKey123456789012345678901A"
SIDECAR="/tmp/et_e2e_sidecar_$$.txt"

cleanup() {
    tmux -S "$SOCK" kill-server 2>/dev/null || true
    kill -9 $(lsof -t -i:4444 2>/dev/null) 2>/dev/null || true
    kill -9 $(lsof -t -i:4445 2>/dev/null) 2>/dev/null || true
    pkill -9 -f "etterminal.*$ID" 2>/dev/null || true
    rm -f "$FIFO" "$SOCK" "$SIDECAR" || true
}
trap cleanup EXIT
cleanup 2>/dev/null

# Start services
tmux -S "$SOCK" new-session -d -s t -x 200 -y 50
tmux -S "$SOCK" send-keys "$BUILD/etserver --serverfifo=$FIFO --port=4444" Enter
sleep 5
tmux -S "$SOCK" split-window -v
sleep 1
tmux -S "$SOCK" send-keys "python3 $DIR/throttle_proxy.py 4445 4444 $RATE" Enter
sleep 3
tmux -S "$SOCK" split-window -v
sleep 1
tmux -S "$SOCK" send-keys "$BUILD/etterminal --idpasskey='$ID/$KEY' --serverfifo=$FIFO" Enter
sleep 5

# Verify
ES=$(lsof -i:4444 2>/dev/null | grep -c LISTEN)
PX=$(lsof -i:4445 2>/dev/null | grep -c LISTEN)
if [ "$ES" -lt 1 ] || [ "$PX" -lt 1 ]; then
    echo "FAILED: etserver=$ES proxy=$PX"
    exit 1
fi

# Connect client
EXTRA=""
if echo "$BUILD/et --help" | grep -q flow-control 2>/dev/null; then
    EXTRA="--flow-control $MODE"
fi
tmux -S "$SOCK" new-window -n et
tmux -S "$SOCK" send-keys -t t:et "$BUILD/et --idpasskey='$ID/$KEY' 127.0.0.1:4445 $EXTRA" Enter
sleep 10

# Verify proxy connection
PROXY_OUT=$(tmux -S "$SOCK" capture-pane -t t:0.1 -p)
if ! echo "$PROXY_OUT" | grep -q connect; then
    echo "FAILED: proxy saw no connection"
    echo "$PROXY_OUT" | tail -5
    exit 1
fi

# Run timestamp test
tmux -S "$SOCK" send-keys -t t:et "SIDECAR_FILE=$SIDECAR python3 $DIR/print_timestamps.py" Enter

echo "=== $MODE mode, ${RATE}B/s proxy ==="
for secs in 10 20 30; do
    sleep 10
    SCREEN=$(tmux -S "$SOCK" capture-pane -t t:et -p | grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]+' | tail -1)
    REAL=$(python3 -c "from datetime import datetime; print(datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f'))")
    if [ -n "$SCREEN" ]; then
        LAG=$(python3 -c "
from datetime import datetime
s=datetime.strptime('${SCREEN}'.strip(),'%Y-%m-%d %H:%M:%S.%f')
r=datetime.strptime('${REAL}'.strip(),'%Y-%m-%d %H:%M:%S.%f')
print(f'{(r-s).total_seconds():.1f}')")
        echo "  t=${secs}s: display_lag=${LAG}s"
    else
        echo "  t=${secs}s: no data on screen"
    fi

    # Check if the process is stalled by reading the sidecar
    if [ -f "$SIDECAR" ]; then
        PROCESS_TS=$(cat "$SIDECAR")
        PROCESS_LAG=$(python3 -c "
from datetime import datetime
s=datetime.strptime('${PROCESS_TS}'.strip(),'%Y-%m-%d %H:%M:%S.%f')
r=datetime.strptime('${REAL}'.strip(),'%Y-%m-%d %H:%M:%S.%f')
print(f'{(r-s).total_seconds():.1f}')")
        echo "           process_lag=${PROCESS_LAG}s (0=running, >1=stalled)"
    fi
done

echo ""
echo "Proxy stats:"
tmux -S "$SOCK" capture-pane -t t:0.1 -p | grep -E 'sent=|connect' | tail -3
echo "etserver alive: $(lsof -i:4444 2>/dev/null | grep -c LISTEN)"
