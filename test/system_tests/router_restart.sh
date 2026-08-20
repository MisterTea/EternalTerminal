#!/bin/bash
# router_restart.sh — Phase 2 contract: an etserver (router) restart must not
# kill sessions.  Two named sessions keep their remote etterminal processes
# (same pids), the clients reconnect by themselves, and shell state survives.
set -x
set -e

cd "$(dirname "$0")/../.."

ET_PORT=9923
ET_FIFO=/tmp/et_router_restart.fifo
TEST_HOME=$(mktemp -d /tmp/et_router_home_XXXXXXXX)
LOG_DIR=/tmp/et_test_logs/router_restart
SERVER_LOG_DIR=$LOG_DIR/server

server_pid=""
declare -a client_pids=()

cleanup() {
  for pid in "${client_pids[@]}"; do
    kill -9 "$pid" 2>/dev/null || true
  done
  [ -n "$server_pid" ] && kill -9 "$server_pid" 2>/dev/null || true
  pkill -9 -f "etterminal.*--serverfifo=$ET_FIFO" 2>/dev/null || true
  [ -n "$KEEP_LOGS" ] || rm -rf "$TEST_HOME" "$LOG_DIR" || true
}
trap cleanup EXIT

wait_for_file() { # path, seconds
  for _ in $(seq 1 "$(( $2 * 10 ))"); do
    [ -f "$1" ] && return 0
    sleep 0.1
  done
  echo "timed out waiting for file $1" >&2
  return 1
}

wait_for_grep() { # pattern, file, seconds
  for _ in $(seq 1 "$(( $3 * 10 ))"); do
    grep -q "$1" "$2" 2>/dev/null && return 0
    sleep 0.1
  done
  echo "timed out waiting for '$1' in $2" >&2
  return 1
}

terminal_pids() {
  pgrep -f "etterminal.*--serverfifo=$ET_FIFO" | sort -n | tr '\n' ' '
}

ssh -o 'PreferredAuthentications=publickey' localhost "echo" || exit 1
ssh -o "StrictHostKeyChecking no" localhost echo "Bypassing host check"

# Reap leftover fixture terminals from earlier aborted runs (with router
# recovery they retry re-registration forever by design).
pkill -9 -f "etterminal.*--serverfifo=$ET_FIFO" 2>/dev/null || true
rm -rf "$LOG_DIR"

mkdir -p "$SERVER_LOG_DIR"
build/etserver --port $ET_PORT --serverfifo=$ET_FIFO -l "$SERVER_LOG_DIR" &
server_pid=$!
sleep 3

# Two named sessions, each with a held-open stdin fifo and a sentinel.
# script(1) gives each client a real pty for stdin (a non-tty console has
# its input path disabled).
for name in one two; do
  mkfifo "$LOG_DIR/input_$name"
  eval "exec 1$([ "$name" = one ] && echo 1 || echo 2)<>$LOG_DIR/input_$name"
  HOME=$TEST_HOME script -qec "build/et --name $name --serverfifo=$ET_FIFO \
    --terminal-path $PWD/build/etterminal --logtostdout \
    localhost:$ET_PORT" /dev/null <"$LOG_DIR/input_$name" \
    >"$LOG_DIR/client_$name.log" 2>&1 &
  client_pids+=($!)
  wait_for_file "$TEST_HOME/.et/sessions/$name" 30
done

printf 'S_ONE=first\n' >&11
printf 'S_TWO=second\n' >&12
printf 'echo READY-ONE\n' >&11
printf 'echo READY-TWO\n' >&12
wait_for_grep 'READY-ONE' "$LOG_DIR/client_one.log" 30
wait_for_grep 'READY-TWO' "$LOG_DIR/client_two.log" 30

pids_before=$(terminal_pids)
[ -n "$pids_before" ] || {
  echo "no fixture etterminal processes found" >&2
  exit 1
}
count_before=$(echo $pids_before | wc -w)
[ "$count_before" -eq 2 ] || {
  echo "expected 2 fixture etterminals, found: $pids_before" >&2
  exit 1
}

# Restart the router: SIGTERM, then relaunch the identical command.
kill -TERM "$server_pid"
wait "$server_pid" 2>/dev/null || true
server_pid=""
sleep 2
build/etserver --port $ET_PORT --serverfifo=$ET_FIFO -l "$SERVER_LOG_DIR" &
server_pid=$!

# The etterminal processes (the shells) survive with identical pids.
for _ in $(seq 1 600); do
  pids_after=$(terminal_pids)
  [ "$pids_after" = "$pids_before" ] && break
  sleep 0.1
done
[ "$pids_after" = "$pids_before" ] || {
  echo "etterminal pids changed: '$pids_before' -> '$pids_after'" >&2
  exit 1
}

# Both clients reconnect on their own and the shell state survived.
wait_for_grep 'Reconnect complete\|Reconnected' "$LOG_DIR/client_one.log" 60 || true
printf 'echo POST-ONE-$S_ONE\n' >&11
printf 'echo POST-TWO-$S_TWO\n' >&12
wait_for_grep 'POST-ONE-first' "$LOG_DIR/client_one.log" 60
wait_for_grep 'POST-TWO-second' "$LOG_DIR/client_two.log" 60

# --attach still works against the restarted server.
HOME=$TEST_HOME build/et --attach one --serverfifo=$ET_FIFO \
  --terminal-path "$PWD/build/etterminal" --logtostdout \
  -c 'echo ATTACH-$S_ONE' >"$LOG_DIR/attach.log" 2>&1 || true
wait_for_grep 'ATTACH-first' "$LOG_DIR/attach.log" 60

# Clean teardown of the remaining session.
printf 'exit\n' >&12
for _ in $(seq 1 100); do
  [ ! -f "$TEST_HOME/.et/sessions/two" ] && break
  sleep 0.1
done

echo "router_restart.sh: OK"
