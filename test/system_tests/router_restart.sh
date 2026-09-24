#!/bin/bash
# shellcheck disable=SC2016 # $-expressions expand in the remote shell
# Restarting etserver keeps sessions alive: etterminal (and its shell) keeps
# running, a connected client reconnects by itself, and a saved session can
# still be attached afterwards.
ET_PORT=9950
# shellcheck source=test/system_tests/session_test_lib.sh
source "$(dirname "$0")/session_test_lib.sh"
start_server

start_client 10 "$LOGS/one.log" --name one "localhost:$ET_PORT"
wait_for 30 test -f "$SESSIONS/one"
printf 'S_ONE=first; echo READY-$S_ONE\n' >&10
wait_for 30 grep -q READY-first "$LOGS/one.log"
pids_before=$(terminal_pids)
[ -n "$pids_before" ]

pkill -TERM -f -- "etserver --port $ET_PORT --serverfifo=$ET_FIFO"
wait_for 10 no_process "etserver --port $ET_PORT --serverfifo=$ET_FIFO"
start_server
[ "$(terminal_pids)" = "$pids_before" ]

# The live client reconnects and the shell state survived. Input typed before
# the reset recovery completes is not replayed, so wait for it.
wait_for 60 grep -q 'Reconnect complete' "$LOGS/one.log"
printf 'echo POST-$S_ONE\n' >&10
wait_for 60 grep -q POST-first "$LOGS/one.log"

# Attach after both the server and the client restarted.
kill_client --name one
$ET --attach one -c 'echo ATTACH-$S_ONE' >"$LOGS/attach.log" 2>&1 || true
grep -q ATTACH-first "$LOGS/attach.log"

# A pipe (-T) session cannot be resumed by a new server: it ends, and its
# whole process group goes with it.
timeout 90 $ET -T -c 'sleep 617 | cat' "localhost:$ET_PORT" </dev/null \
  >"$LOGS/pipe.log" 2>&1 &
pipe_client=$!
wait_for 30 pgrep -f 'sleep 617'
pkill -TERM -f -- "etserver --port $ET_PORT --serverfifo=$ET_FIFO"
wait_for 10 no_process "etserver --port $ET_PORT --serverfifo=$ET_FIFO"
wait_for 10 no_process 'sleep 617'
start_server
pipe_status=0
wait "$pipe_client" || pipe_status=$?
[ "$pipe_status" -ne 124 ]

echo "router_restart.sh: OK"
