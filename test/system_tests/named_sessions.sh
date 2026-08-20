#!/bin/bash
# named_sessions.sh — Phase 1 contract: a named session survives the client
# process dying (laptop reboot proxy) and can be reattached with --attach.
set -x
set -e

cd "$(dirname "$0")/../.."

ET_PORT=9922
ET_FIFO=/tmp/et_named_sessions.fifo
TEST_HOME=$(mktemp -d /tmp/et_named_home_XXXXXXXX)
LOG_DIR=/tmp/et_test_logs/named_sessions
SERVER_LOG_DIR=$LOG_DIR/server
CLIENT_LOG=$LOG_DIR/client.log
ATTACH_LOG=$LOG_DIR/attach.log
INPUT_FIFO=$LOG_DIR/input_fifo
ATTACH_FIFO=$LOG_DIR/attach_fifo

server_pid=""
client_pid=""
attach_pid=""

cleanup() {
  [ -n "$attach_pid" ] && kill -9 "$attach_pid" 2>/dev/null || true
  [ -n "$client_pid" ] && kill -9 "$client_pid" 2>/dev/null || true
  [ -n "$server_pid" ] && kill -9 "$server_pid" 2>/dev/null || true
  pkill -9 -f "etterminal.*--serverfifo=$ET_FIFO" 2>/dev/null || true
  rm -rf "$TEST_HOME" "$LOG_DIR" || true
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

mkfifo "$INPUT_FIFO" "$ATTACH_FIFO"
# Hold the fifos open so the clients never see EOF on stdin.
exec 9<>"$INPUT_FIFO"
exec 10<>"$ATTACH_FIFO"

# Start a named session in the background.  script(1) gives the client a
# real pty for stdin: without a tty the console input path disables itself
# and no keystrokes would reach the session.
HOME=$TEST_HOME script -qec "build/et --name alpha --serverfifo=$ET_FIFO \
  --terminal-path $PWD/build/etterminal --logtostdout \
  localhost:$ET_PORT" /dev/null <"$INPUT_FIFO" >"$CLIENT_LOG" 2>&1 &
client_pid=$!

# The session file is written once the initial connect succeeds.
wait_for_file "$TEST_HOME/.et/sessions/alpha" 30

# The session works: set a sentinel variable in the remote shell.
printf 'ET_SENTINEL=abc123\n' >&9
printf 'echo PRE-$((6*7))\n' >&9
wait_for_grep 'PRE-42' "$CLIENT_LOG" 30

# Simulate a laptop reboot: SIGKILL the client (the script wrapper and the
# et process under it).  The session file must stay.
pkill -9 -P "$client_pid" 2>/dev/null || true
kill -9 "$client_pid" 2>/dev/null || true
pkill -9 -f "build/et --name alpha" 2>/dev/null || true
wait "$client_pid" 2>/dev/null || true
client_pid=""
[ -f "$TEST_HOME/.et/sessions/alpha" ] || {
  echo "session file vanished after client kill" >&2
  exit 1
}

# --list shows the session without connecting.
HOME=$TEST_HOME build/et --list | grep -q alpha

# --attach reattaches to the same remote shell: the sentinel is still set.
HOME=$TEST_HOME script -qec "build/et --attach alpha --serverfifo=$ET_FIFO \
  --terminal-path $PWD/build/etterminal --logtostdout" /dev/null \
  <"$ATTACH_FIFO" >"$ATTACH_LOG" 2>&1 &
attach_pid=$!
sleep 5
printf 'echo POST-$ET_SENTINEL\n' >&10
wait_for_grep 'POST-abc123' "$ATTACH_LOG" 30

# A clean shell exit ends the session and drops the file.
printf 'exit\n' >&10
for _ in $(seq 1 100); do
  [ ! -f "$TEST_HOME/.et/sessions/alpha" ] && break
  sleep 0.1
done
[ ! -f "$TEST_HOME/.et/sessions/alpha" ] || {
  echo "session file not removed after clean exit" >&2
  exit 1
}
wait "$attach_pid" 2>/dev/null || true
attach_pid=""

echo "named_sessions.sh: OK"
