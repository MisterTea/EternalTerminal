#!/bin/bash
# Runs one E2E flow control scenario. Outputs JSONL metrics.
#
# Usage: bash do_scenario.sh <mode> [--disconnect] [--outdir DIR]
#   mode: trunk | backpressure | discard
#   --disconnect: simulate 60s TCP disconnect at t=30s
#   --outdir: directory for JSONL output and logs
#
# Commits:
#   trunk:        ee8ddc21c (base) + f8930e169 (harness: --idpasskey, crash fix)
#   backpressure: 497000e08 (--flow-control backpressure)
#   discard:      497000e08 (--flow-control discard)

MODE="$1"; shift
DISCONNECT=false
OUTDIR="/tmp/et_e2e_results"
while [ $# -gt 0 ]; do
    case "$1" in
        --disconnect) DISCONNECT=true ;;
        --outdir) OUTDIR="$2"; shift ;;
    esac
    shift
done

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
BUILD="$REPO/build"
RATE=100000
ID="E2E$(date +%s | tail -c 8)"
KEY="E2ETestKey123456789012345678901A"
SIDECAR="/tmp/et_e2e_sidecar_$$.txt"
ETMUX="/tmp/et_e2e_client_$$.sock"
PROXY_LOG="/tmp/et_e2e_proxy_$$.log"

# The cleanup trap restores src/, proto/, and test dirs to HEAD (the trunk
# scenario checks out old sources there). Any uncommitted work in those
# paths would be silently destroyed, so refuse to run on a dirty tree.
if ! git -C "$REPO" diff --quiet -- src/ proto/ test/integration_tests/ test/unit_tests/ 2>/dev/null; then
    echo "ERROR: uncommitted changes under src/, proto/, or test/. Commit them first:"
    git -C "$REPO" status --short -- src/ proto/ test/integration_tests/ test/unit_tests/ | head
    exit 1
fi

mkdir -p "$OUTDIR"

SCENARIO="$MODE"
[ "$DISCONNECT" = "true" ] && SCENARIO="${MODE}-disconnect"
JSONL="$OUTDIR/${SCENARIO}.jsonl"
> "$JSONL"

C='\033[1;36m'; G='\033[1;32m'; R='\033[1;31m'; Y='\033[33m'; N='\033[0m'

cleanup() {
    tmux -S "$ETMUX" kill-server 2>/dev/null
    kill $ETSERVER_PID $PROXY_PID 2>/dev/null
    kill -9 $(lsof -t -i:4444 2>/dev/null) $(lsof -t -i:4445 2>/dev/null) 2>/dev/null
    pkill -9 -f "etterminal.*$ID" 2>/dev/null
    rm -f /tmp/et_e2e_demo.fifo "$ETMUX" "$SIDECAR" "$PROXY_LOG"
    # Restore only the dirs the trunk scenario checks out. Do NOT use
    # "git checkout HEAD -- ." here: it clobbers unrelated uncommitted work
    # in the tree (this exact mistake once reverted the flow-control
    # implementation itself).
    cd "$REPO" && git checkout HEAD -- src/ proto/ test/integration_tests/ test/unit_tests/ 2>/dev/null
}
trap cleanup EXIT

# --- Checkout and build ---

COMMIT_DESC=""
ET_CMD=""
case "$MODE" in
    trunk)
        echo -e "${C}=== Scenario: trunk (no flow control) ===${N}"
        COMMIT_DESC="ee8ddc21c + f8930e169 (harness only, no WriteBuffer)"
        ET_CMD="./et --idpasskey=ID/KEY 127.0.0.1:4445"
        cd "$REPO"
        git checkout ee8ddc21c -- src/ proto/ test/integration_tests/ test/unit_tests/ 2>/dev/null
        git checkout f8930e169 -- src/terminal/TerminalClientMain.cpp src/terminal/TerminalMain.cpp src/base/Headers.hpp 2>/dev/null
        rm -f src/base/WriteBuffer.hpp test/unit_tests/WriteBufferTest.cpp
        ;;
    none)
        echo -e "${C}=== Scenario: none (HEAD, no --flow-control: must match trunk) ===${N}"
        COMMIT_DESC="HEAD ($(cd "$REPO" && git rev-parse --short HEAD)), no flag"
        ET_CMD="./et --idpasskey=ID/KEY 127.0.0.1:4445"
        # Use HEAD as-is (no checkout needed)
        ;;
    backpressure)
        echo -e "${C}=== Scenario: backpressure ===${N}"
        COMMIT_DESC="HEAD ($(cd "$REPO" && git rev-parse --short HEAD))"
        ET_CMD="./et --idpasskey=ID/KEY --flow-control backpressure 127.0.0.1:4445"
        # Use HEAD as-is (no checkout needed)
        ;;
    discard)
        echo -e "${C}=== Scenario: discard ===${N}"
        COMMIT_DESC="HEAD ($(cd "$REPO" && git rev-parse --short HEAD))"
        ET_CMD="./et --idpasskey=ID/KEY --flow-control discard 127.0.0.1:4445"
        # Use HEAD as-is (no checkout needed)
        ;;
    *) echo "Usage: $0 <trunk|none|backpressure|discard> [--disconnect]"; exit 1 ;;
esac
FC_FLAG=""
[ "$MODE" = "backpressure" ] && FC_FLAG="--flow-control backpressure"
[ "$MODE" = "discard" ] && FC_FLAG="--flow-control discard"

echo "  commit: $COMMIT_DESC"
echo "  client: $ET_CMD"
echo "  proxy:  ${RATE}B/s throttle on port 4445"
[ "$DISCONNECT" = "true" ] && echo "  disconnect: 60s at t=30s"
echo ""

echo -e "${C}--- Build ---${N}"
cd "$BUILD"
cmake -DDISABLE_VCPKG=ON -GNinja .. 2>&1 | tail -1
ninja -j4 2>&1 | tail -3
echo ""

# --- Start services ---

echo -e "${C}--- Services ---${N}"
rm -f /tmp/et_e2e_demo.fifo

"$BUILD/etserver" --serverfifo=/tmp/et_e2e_demo.fifo --port=4444 --logdir="$OUTDIR" &>/dev/null &
ETSERVER_PID=$!
sleep 3

PYTHONUNBUFFERED=1 python3 -u "$DIR/throttle_proxy.py" 4445 4444 "$RATE" >"$PROXY_LOG" 2>&1 &
PROXY_PID=$!
sleep 2

"$BUILD/etterminal" --idpasskey="$ID/$KEY" --serverfifo=/tmp/et_e2e_demo.fifo --logdir="$OUTDIR" &>/dev/null &
sleep 3

for i in $(seq 1 15); do
    ES=$(lsof -i:4444 2>/dev/null | grep -c LISTEN)
    PX=$(lsof -i:4445 2>/dev/null | grep -c LISTEN)
    [ "$ES" -ge 1 ] && [ "$PX" -ge 1 ] && break
    sleep 1
done
[ "$ES" -lt 1 ] || [ "$PX" -lt 1 ] && { echo -e "${R}FAILED: etserver=$ES proxy=$PX${N}"; exit 1; }
echo -e "${G}etserver=:4444 proxy=:4445 (${RATE}B/s)${N}"

# --- Connect ---

echo -e "${C}--- Connect ---${N}"
tmux -S "$ETMUX" new-session -d -s et -x 200 -y 50
for i in $(seq 1 30); do
    tmux -S "$ETMUX" capture-pane -p 2>/dev/null | grep -q '\$' && break
    sleep 1
done
tmux -S "$ETMUX" send-keys "$BUILD/et --idpasskey='$ID/$KEY' 127.0.0.1:4445 $FC_FLAG" Enter

for i in $(seq 1 30); do
    grep -q connect "$PROXY_LOG" 2>/dev/null && break
    sleep 1
    [ "$i" -eq 30 ] && { echo -e "${R}FAILED: no proxy connection${N}"; cat "$PROXY_LOG"; exit 1; }
done
echo -e "${G}Connected through proxy${N}"

for i in $(seq 1 30); do
    tmux -S "$ETMUX" capture-pane -p 2>/dev/null | grep -qF '~]$' && break
    sleep 1
done
echo -e "${G}Remote shell ready${N}"
echo ""

# --- Run workload ---

TOTAL_DURATION=30
[ "$DISCONNECT" = "true" ] && TOTAL_DURATION=120
DISCONNECT_AT=30
RECONNECT_AT=90

echo -e "${C}--- print_timestamps.py (${TOTAL_DURATION}s) ---${N}"
tmux -S "$ETMUX" send-keys "SIDECAR_FILE=$SIDECAR python3 $DIR/print_timestamps.py" Enter

PREV_LINES=0
PREV_ELAPSED="0"

printf "%-6s  %-12s  %-12s  %-12s  %-10s  %-10s\n" "TIME" "DISPLAY_LAG" "PROCESS_LAG" "LINES/SEC" "CONNECTED" "STATUS"
printf "%-6s  %-12s  %-12s  %-12s  %-10s  %-10s\n" "----" "-----------" "-----------" "---------" "---------" "------"

ELAPSED=0
while [ "$ELAPSED" -lt "$TOTAL_DURATION" ]; do
    sleep 5
    ELAPSED=$((ELAPSED + 5))

    if [ "$DISCONNECT" = "true" ]; then
        [ "$ELAPSED" -eq "$DISCONNECT_AT" ] && { echo -e "${R}>>> DISCONNECT <<<${N}"; kill -USR1 $PROXY_PID 2>/dev/null; }
        [ "$ELAPSED" -eq "$RECONNECT_AT" ] && { echo -e "${G}>>> RECONNECT <<<${N}"; kill -USR2 $PROXY_PID 2>/dev/null; }
    fi

    CONNECTED="true"
    [ "$DISCONNECT" = "true" ] && [ "$ELAPSED" -gt "$DISCONNECT_AT" ] && [ "$ELAPSED" -le "$RECONNECT_AT" ] && CONNECTED="false"

    SCREEN=$(tmux -S "$ETMUX" capture-pane -p 2>/dev/null | grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]+' | tail -1)
    REAL=$(python3 -c "from datetime import datetime; print(datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f'))")

    DLAG="null"
    [ -n "$SCREEN" ] && DLAG=$(python3 -c "
from datetime import datetime
s=datetime.strptime('${SCREEN}'.strip(),'%Y-%m-%d %H:%M:%S.%f')
r=datetime.strptime('${REAL}'.strip(),'%Y-%m-%d %H:%M:%S.%f')
print(f'{(r-s).total_seconds():.1f}')" 2>/dev/null || true)

    PLAG="null"; LINES_SEC="null"; CUR_LINES=0; CUR_ELAPSED="0"
    if [ -f "$SIDECAR" ]; then
        IFS=',' read -r PROCESS_TS CUR_LINES CUR_ELAPSED < "$SIDECAR"
        [ -n "$PROCESS_TS" ] && PLAG=$(python3 -c "
from datetime import datetime
s=datetime.strptime('${PROCESS_TS}'.strip(),'%Y-%m-%d %H:%M:%S.%f')
r=datetime.strptime('${REAL}'.strip(),'%Y-%m-%d %H:%M:%S.%f')
print(f'{(r-s).total_seconds():.1f}')" 2>/dev/null || true)
        [ -n "$CUR_ELAPSED" ] && [ -n "$CUR_LINES" ] && LINES_SEC=$(python3 -c "
cl=int('${CUR_LINES}'); pl=int('${PREV_LINES}')
ct=float('${CUR_ELAPSED}'); pt=float('${PREV_ELAPSED}')
dt=ct-pt
print(f'{(cl-pl)/dt:.0f}' if dt > 0.1 else '0')" 2>/dev/null || true)
    fi
    PREV_LINES="${CUR_LINES:-0}"; PREV_ELAPSED="${CUR_ELAPSED:-0}"

    PSTATUS="running"
    [ "$PLAG" != "null" ] && ! python3 -c "exit(0 if float('$PLAG') < 1.0 else 1)" 2>/dev/null && PSTATUS="STALLED"

    DLAG_S="${DLAG}s"; [ "$DLAG" = "null" ] && DLAG_S="?"
    PLAG_S="${PLAG}s"; [ "$PLAG" = "null" ] && PLAG_S="?"
    LS_S="$LINES_SEC"; [ "$LINES_SEC" = "null" ] && LS_S="?"

    COLOR="$G"; [ "$PSTATUS" = "STALLED" ] && COLOR="$R"
    printf "t=%-3ss  %-12s  %-12s  %-12s  %-10s  ${COLOR}%-10s${N}\n" "$ELAPSED" "$DLAG_S" "$PLAG_S" "$LS_S" "$CONNECTED" "$PSTATUS"

    echo "{\"t\":$ELAPSED,\"display_lag\":$DLAG,\"process_lag\":$PLAG,\"lines_sec\":$LINES_SEC,\"connected\":$CONNECTED}" >> "$JSONL"
done

# --- Ctrl-C responsiveness ---
# This is the original issue 631 complaint: with a saturated link, ^C takes
# a very long time to visibly take effect because stale queued output must
# drain before the prompt reappears.
echo ""
echo -e "${C}--- Ctrl-C responsiveness ---${N}"
CC_T0=$(python3 -c 'import time; print(f"{time.time():.3f}")')
tmux -S "$ETMUX" send-keys C-c
CTRL_C_LATENCY="null"
for i in $(seq 1 240); do
    sleep 0.5
    LAST_LINES=$(tmux -S "$ETMUX" capture-pane -p 2>/dev/null | sed '/^$/d' | tail -2)
    if echo "$LAST_LINES" | grep -qF '~]$'; then
        CTRL_C_LATENCY=$(python3 -c "import time; print(f'{time.time() - $CC_T0:.1f}')")
        break
    fi
done
if [ "$CTRL_C_LATENCY" = "null" ]; then
    echo -e "${R}Ctrl-C: prompt did not return within 120s${N}"
else
    echo -e "Ctrl-C to prompt: ${G}${CTRL_C_LATENCY}s${N}"
fi
echo "{\"event\":\"ctrl_c\",\"latency\":$CTRL_C_LATENCY}" >> "$JSONL"

echo ""
ES_ALIVE=$(lsof -i:4444 2>/dev/null | grep -c LISTEN)
[ "$ES_ALIVE" -gt 0 ] && echo -e "etserver: ${G}alive${N}" || echo -e "etserver: ${R}CRASHED${N}"

cp "$PROXY_LOG" "$OUTDIR/${SCENARIO}_proxy.log" 2>/dev/null

python3 -c "
import json
meta = {
    'scenario': '$SCENARIO',
    'mode': '$MODE',
    'disconnect': $( [ \"$DISCONNECT\" = \"true\" ] && echo True || echo False ),
    'commit': '$COMMIT_DESC',
    'et_command': '$ET_CMD',
    'proxy_rate': $RATE,
    'ctrl_c_latency': $CTRL_C_LATENCY,
    'etserver_alive': $( [ "$ES_ALIVE" -gt 0 ] && echo True || echo False ),
}
with open('$OUTDIR/${SCENARIO}_meta.json', 'w') as f:
    json.dump(meta, f, indent=2)
"

echo "Results: $JSONL"

tmux -S "$ETMUX" send-keys C-c 2>/dev/null
sleep 1
tmux -S "$ETMUX" send-keys "exit" Enter 2>/dev/null
sleep 1
