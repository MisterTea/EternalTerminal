#!/bin/bash
# named_sessions.sh — Phase 1 contract: a named session survives the client
# process dying (laptop reboot proxy) and can be reattached with --attach.
set -x
set -e

cd "$(dirname "$0")/../.."

ET_PORT=9922
ET_FIFO=/tmp/et_named_sessions.fifo
TEST_HOME=$(mktemp -d /tmp/et_named_home_XXXXXXXX)
DIAGNOSTIC_START=$TEST_HOME/diagnostic_start
LOG_DIR=/tmp/et_test_logs/named_sessions
SERVER_LOG_DIR=$LOG_DIR/server
CLIENT_LOG=$LOG_DIR/client.log
ATTACH_LOG=$LOG_DIR/attach.log
NAME_LOG=$LOG_DIR/name.log
MISMATCH_LOG=$LOG_DIR/mismatch.log
RECREATE_LOG=$LOG_DIR/recreate.log
AMBIGUOUS_LOG=$LOG_DIR/ambiguous.log
UNNAMED_LOG=$LOG_DIR/unnamed.log
NO_PERSIST_LOG=$LOG_DIR/no-persist.log
UNWRITABLE_LIST_LOG=$LOG_DIR/unwritable-list.log
UNWRITABLE_NAME_LOG=$LOG_DIR/unwritable-name.log
ATTACH_OPTIONS_LOG=$LOG_DIR/attach-options.log
KILL_LOG=$LOG_DIR/kill.log
KILL_ATTACH_LOG=$LOG_DIR/kill-attach.log
STALE_KILL_LOG=$LOG_DIR/stale-kill.log
UNREACHABLE_KILL_LOG=$LOG_DIR/unreachable-kill.log
INPUT_FIFO=$LOG_DIR/input_fifo
ATTACH_FIFO=$LOG_DIR/attach_fifo

server_pid=""
client_pid=""
attach_pid=""
aux_pid=""

dump_logs() {
  echo "named_sessions.sh: failure diagnostics (last 60 lines per log)" >&2
  while IFS= read -r -d '' log; do
    echo "===== $log =====" >&2
    tail -n 60 "$log" >&2 || true
  done < <(find "$LOG_DIR" -type f -name '*.log' -print0 2>/dev/null | sort -z)
  while IFS= read -r -d '' log; do
    echo "===== $log =====" >&2
    tail -n 60 "$log" >&2 || true
  done < <(find /tmp -maxdepth 1 -type f -name 'etterminal-*.log' \
    -newer "$DIAGNOSTIC_START" -print0 2>/dev/null | sort -z)
}

cleanup() {
  status=$?
  trap - EXIT
  [ "$status" -eq 0 ] || dump_logs
  [ -n "$attach_pid" ] && kill -9 "$attach_pid" 2>/dev/null || true
  [ -n "$aux_pid" ] && kill -9 "$aux_pid" 2>/dev/null || true
  [ -n "$client_pid" ] && kill -9 "$client_pid" 2>/dev/null || true
  [ -n "$server_pid" ] && kill -9 "$server_pid" 2>/dev/null || true
  pkill -9 -f "etterminal.*--serverfifo=$ET_FIFO" 2>/dev/null || true
  [ -n "$KEEP_LOGS" ] || rm -rf "$TEST_HOME" "$LOG_DIR" || true
  exit "$status"
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

wait_for_dir() { # path, seconds
  for _ in $(seq 1 "$(( $2 * 10 ))"); do
    [ -d "$1" ] && return 0
    sleep 0.1
  done
  echo "timed out waiting for directory $1" >&2
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

wait_for_process_exit() { # pid, seconds
  for _ in $(seq 1 "$(( $2 * 10 ))"); do
    kill -0 "$1" 2>/dev/null || return 0
    sleep 0.1
  done
  echo "timed out waiting for process $1 to exit" >&2
  return 1
}

ssh -o 'PreferredAuthentications=publickey' localhost "echo" || exit 1
ssh -o "StrictHostKeyChecking no" localhost echo "Bypassing host check"

# Reap leftover fixture terminals from earlier aborted runs (with router
# recovery they retry re-registration forever by design).
pkill -9 -f "etterminal.*--serverfifo=$ET_FIFO" 2>/dev/null || true
rm -rf "$LOG_DIR"

touch "$DIAGNOSTIC_START"
mkdir -p "$SERVER_LOG_DIR"
build/etserver --port $ET_PORT --serverfifo=$ET_FIFO -l "$SERVER_LOG_DIR" &
server_pid=$!
sleep 3

# Attach uses only the saved endpoint and credentials. Options that require a
# fresh SSH bootstrap must fail explicitly instead of being ignored.
for option in tunnel reverse agent jumphost; do
  case "$option" in
    tunnel) args=(-t 18080:80) ;;
    reverse) args=(-r 18081:81) ;;
    agent) args=(-f) ;;
    jumphost) args=(-j localhost) ;;
  esac
  if HOME=$TEST_HOME build/et --attach alpha "${args[@]}" \
    >"$ATTACH_OPTIONS_LOG" 2>&1; then
    echo "--attach unexpectedly accepted $option options" >&2
    exit 1
  fi
  grep -F -q -- "--attach cannot be combined with -t, -r, -f, or -j" \
    "$ATTACH_OPTIONS_LOG"
done

# Ordinary sessions are persisted under an automatically generated,
# human-readable name. The name is intentionally independent of the session
# credentials returned by the SSH bootstrap.
HOME=$TEST_HOME build/et -N --serverfifo=$ET_FIFO \
  --terminal-path "$PWD/build/etterminal" --logtostdout \
  "localhost:$ET_PORT" >"$UNNAMED_LOG" 2>&1 &
aux_pid=$!
wait_for_grep 'ET running, feel free to background' "$UNNAMED_LOG" 30
wait_for_dir "$TEST_HOME/.et/sessions" 30
default_count=$(find "$TEST_HOME/.et/sessions" -maxdepth 1 -type f | wc -l | tr -d ' ')
[ "$default_count" -eq 1 ] || {
  echo "ordinary session did not create exactly one saved session" >&2
  exit 1
}
HOME=$TEST_HOME build/et --list | grep -E -q '^localhost-20[0-9]{6}-[0-9]{6}-[[:alnum:]]{4}'
kill -9 "$aux_pid" 2>/dev/null || true
wait "$aux_pid" 2>/dev/null || true
aux_pid=""
pkill -9 -f "etterminal.*--serverfifo=$ET_FIFO" 2>/dev/null || true

# --no-persist opts out without deleting already persisted sessions.
HOME=$TEST_HOME build/et --no-persist -N --serverfifo=$ET_FIFO \
  --terminal-path "$PWD/build/etterminal" --logtostdout \
  "localhost:$ET_PORT" >"$NO_PERSIST_LOG" 2>&1 &
aux_pid=$!
wait_for_grep 'ET running, feel free to background' "$NO_PERSIST_LOG" 30
no_persist_count=$(find "$TEST_HOME/.et/sessions" -maxdepth 1 -type f | wc -l | tr -d ' ')
[ "$no_persist_count" -eq 1 ] || {
  echo "--no-persist changed the saved session set" >&2
  exit 1
}
kill -9 "$aux_pid" 2>/dev/null || true
wait "$aux_pid" 2>/dev/null || true
aux_pid=""
pkill -9 -f "etterminal.*--serverfifo=$ET_FIFO" 2>/dev/null || true

if HOME=$TEST_HOME build/et --no-persist --name forbidden \
  >"$ATTACH_OPTIONS_LOG" 2>&1; then
  echo "--no-persist unexpectedly accepted --name" >&2
  exit 1
fi
grep -F -q -- "--no-persist cannot be combined with --name" \
  "$ATTACH_OPTIONS_LOG"
if HOME=$TEST_HOME build/et --no-persist --attach forbidden \
  >"$ATTACH_OPTIONS_LOG" 2>&1; then
  echo "--no-persist unexpectedly accepted --attach" >&2
  exit 1
fi
grep -F -q -- "--no-persist cannot be combined with --name, --attach" \
  "$ATTACH_OPTIONS_LOG"

# Session storage failures are warnings. Listing remains a successful local
# operation, and --name continues as an unnamed live session.
RESTRICTED_HOME=$TEST_HOME/restricted
mkdir "$RESTRICTED_HOME"
chmod 000 "$RESTRICTED_HOME"
HOME=$RESTRICTED_HOME build/et --list >"$UNWRITABLE_LIST_LOG" 2>&1
grep -F -q 'Could not list sessions' "$UNWRITABLE_LIST_LOG"
HOME=$RESTRICTED_HOME build/et --name unwritable -N \
  --serverfifo=$ET_FIFO --terminal-path "$PWD/build/etterminal" \
  --logtostdout "localhost:$ET_PORT" >"$UNWRITABLE_NAME_LOG" 2>&1 &
aux_pid=$!
wait_for_grep "Could not save session 'unwritable'" \
  "$UNWRITABLE_NAME_LOG" 30
wait_for_grep 'ET running, feel free to background' "$UNWRITABLE_NAME_LOG" 30
kill -9 "$aux_pid" 2>/dev/null || true
wait "$aux_pid" 2>/dev/null || true
aux_pid=""
pkill -9 -f "etterminal.*--serverfifo=$ET_FIFO" 2>/dev/null || true
chmod 700 "$RESTRICTED_HOME"

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

# Record an OSC 2 title without changing the bytes rendered by the client.
printf 'printf "\\033]2;ET_TITLE_TEST\\007"\n' >&9
wait_for_grep '^title=ET_TITLE_TEST$' "$TEST_HOME/.et/sessions/alpha" 30

# A connected client keeps the session fresh for the offline list.
HOME=$TEST_HOME build/et --list | grep -E -q 'alpha.*ET_TITLE_TEST.*now'

# A title substring must be unique. Duplicate the record with a valid second
# name so this local-only resolution path cannot accidentally connect.
cp -p "$TEST_HOME/.et/sessions/alpha" "$TEST_HOME/.et/sessions/beta"
sed -i 's/^name=alpha$/name=beta/' "$TEST_HOME/.et/sessions/beta"
if HOME=$TEST_HOME build/et --attach TITLE_TEST >"$AMBIGUOUS_LOG" 2>&1; then
  echo "ambiguous title unexpectedly attached" >&2
  exit 1
fi
grep -F -q "Multiple saved sessions match 'TITLE_TEST':" "$AMBIGUOUS_LOG"
grep -F -q 'alpha [ET_TITLE_TEST]' "$AMBIGUOUS_LOG"
grep -F -q 'beta [ET_TITLE_TEST]' "$AMBIGUOUS_LOG"
rm "$TEST_HOME/.et/sessions/beta"

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

# --list shows coarse file age without connecting. Backdate the file instead
# of waiting for the 30-second "now" window to expire.
touch -d '5 minutes ago' "$TEST_HOME/.et/sessions/alpha"
HOME=$TEST_HOME build/et --list | grep -E -q 'alpha.*5m ago'

# --attach reattaches to the same remote shell: the sentinel is still set.
HOME=$TEST_HOME script -qec "build/et --attach TITLE_TEST --serverfifo=$ET_FIFO \
  --terminal-path $PWD/build/etterminal --logtostdout" /dev/null \
  <"$ATTACH_FIFO" >"$ATTACH_LOG" 2>&1 &
attach_pid=$!
sleep 5
printf 'echo POST-$ET_SENTINEL\n' >&10
wait_for_grep 'POST-abc123' "$ATTACH_LOG" 30

# Detach again, then --name with the same resolved endpoint reattaches instead
# of rejecting the existing name.
pkill -9 -P "$attach_pid" 2>/dev/null || true
kill -9 "$attach_pid" 2>/dev/null || true
pkill -9 -f "build/et --attach TITLE_TEST" 2>/dev/null || true
wait "$attach_pid" 2>/dev/null || true
attach_pid=""

HOME=$TEST_HOME script -qec "build/et --name alpha --serverfifo=$ET_FIFO \
  --terminal-path $PWD/build/etterminal --logtostdout \
  localhost:$ET_PORT" /dev/null <"$INPUT_FIFO" >"$NAME_LOG" 2>&1 &
client_pid=$!
printf 'echo NAME-$ET_SENTINEL\n' >&9
wait_for_grep 'NAME-abc123' "$NAME_LOG" 30

# The same name cannot silently switch to another saved endpoint.
if HOME=$TEST_HOME build/et --name alpha "localhost:$((ET_PORT + 1))" \
  >"$MISMATCH_LOG" 2>&1; then
  echo "mismatched --name unexpectedly succeeded" >&2
  exit 1
fi
grep -F -q "session alpha is saved for localhost:$ET_PORT; use --attach alpha or a different --name" \
  "$MISMATCH_LOG"

# Keep a copy of the credentials, end the remote shell, and restore that stale
# record. --name must discard it and create a fresh shell.
cp -p "$TEST_HOME/.et/sessions/alpha" "$LOG_DIR/alpha.stale"
printf 'exit\n' >&9
for _ in $(seq 1 100); do
  [ ! -f "$TEST_HOME/.et/sessions/alpha" ] && break
  sleep 0.1
done
[ ! -f "$TEST_HOME/.et/sessions/alpha" ] || {
  echo "session file not removed after clean exit" >&2
  exit 1
}
wait "$client_pid" 2>/dev/null || true
client_pid=""
cp -p "$LOG_DIR/alpha.stale" "$TEST_HOME/.et/sessions/alpha"

HOME=$TEST_HOME script -qec "build/et --name alpha --serverfifo=$ET_FIFO \
  --terminal-path $PWD/build/etterminal --logtostdout \
  localhost:$ET_PORT" /dev/null <"$ATTACH_FIFO" >"$RECREATE_LOG" 2>&1 &
attach_pid=$!
wait_for_grep "Session 'alpha' is no longer running; creating a fresh session" \
  "$RECREATE_LOG" 30
wait_for_file "$TEST_HOME/.et/sessions/alpha" 30
printf 'if [ -z "${ET_SENTINEL+x}" ]; then echo FRESH-UNSET; else echo FRESH-SET; fi\n' >&10
wait_for_grep 'FRESH-UNSET' "$RECREATE_LOG" 30

# Preserve the live credentials for stale and unreachable follow-up cases,
# then terminate the session from a separate client. A unique
# case-insensitive substring resolves the same way as --attach.
cp -p "$TEST_HOME/.et/sessions/alpha" "$LOG_DIR/alpha.kill-stale"
terminal_pid=$(pgrep -f "etterminal.*--serverfifo=$ET_FIFO" | tail -n 1)
HOME=$TEST_HOME build/et --kill LPH >"$KILL_LOG" 2>&1
grep -F -q "Killed session 'alpha'" "$KILL_LOG"
wait_for_process_exit "$terminal_pid" 30
[ ! -f "$TEST_HOME/.et/sessions/alpha" ] || {
  echo "--kill did not remove the live session file" >&2
  exit 1
}
wait "$attach_pid" 2>/dev/null || true
attach_pid=""

if HOME=$TEST_HOME build/et --attach alpha >"$KILL_ATTACH_LOG" 2>&1; then
  echo "--attach found a killed session" >&2
  exit 1
fi
grep -F -q "No saved session named 'alpha'" "$KILL_ATTACH_LOG"

# Restoring the ended session's credentials makes a stale local record. The
# server's INVALID_KEY response removes it successfully.
cp -p "$LOG_DIR/alpha.kill-stale" "$TEST_HOME/.et/sessions/alpha"
HOME=$TEST_HOME build/et --kill alpha >"$STALE_KILL_LOG" 2>&1
grep -F -q "Session 'alpha' was already gone; removed stale record" \
  "$STALE_KILL_LOG"
[ ! -f "$TEST_HOME/.et/sessions/alpha" ]

# An unreachable endpoint must retain the record for a later retry.
cp -p "$LOG_DIR/alpha.kill-stale" "$TEST_HOME/.et/sessions/unreachable"
sed -i 's/^name=alpha$/name=unreachable/' \
  "$TEST_HOME/.et/sessions/unreachable"
sed -i "s/^port=$ET_PORT$/port=$((ET_PORT + 1))/" \
  "$TEST_HOME/.et/sessions/unreachable"
if HOME=$TEST_HOME build/et --kill unreachable \
  >"$UNREACHABLE_KILL_LOG" 2>&1; then
  echo "--kill unexpectedly succeeded for an unreachable host" >&2
  exit 1
fi
grep -F -q "Could not reach the ET server: localhost:$((ET_PORT + 1))" \
  "$UNREACHABLE_KILL_LOG"
grep -F -q "Session 'unreachable' was not removed; retry --kill or delete ~/.et/sessions/unreachable manually" \
  "$UNREACHABLE_KILL_LOG"
[ -f "$TEST_HOME/.et/sessions/unreachable" ]
rm "$TEST_HOME/.et/sessions/unreachable"

echo "named_sessions.sh: OK"
