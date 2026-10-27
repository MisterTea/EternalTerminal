# shellcheck shell=bash
# Shared helpers for the saved-session system tests. Source after setting
# ET_PORT; everything a test starts carries this run's unique $ET_FIFO path,
# so cleanup can match exactly this run's processes.
set -x
set -e
export ET_NO_TELEMETRY=YES
cd "$(dirname "$0")/../.."

RUN_DIR=$(mktemp -d "${TMPDIR:-/tmp}/et_sessions.XXXXXXXX")
ET_FIFO=$RUN_DIR/etserver.fifo
LOGS=$RUN_DIR/logs
export HOME=$RUN_DIR/home
# shellcheck disable=SC2034 # used by the sourcing tests
SESSIONS=$HOME/.et/sessions
mkdir -p "$HOME" "$LOGS/server" "$LOGS/terminal"
cat >"$RUN_DIR/etterminal" <<EOF
#!/bin/sh
exec "$PWD/build/etterminal" --logdir="$LOGS/terminal" "\$@"
EOF
chmod 700 "$RUN_DIR/etterminal"
ET="build/et --serverfifo=$ET_FIFO --terminal-path $RUN_DIR/etterminal --logtostdout"

cleanup() {
  local status=$?
  set +e
  if [ "$status" -ne 0 ]; then
    find "$LOGS" -type f -name '*.log' -exec tail -n 40 {} + >&2
  fi
  pkill -KILL -f -- "--serverfifo=$ET_FIFO"
  rm -rf -- "$RUN_DIR"
  exit "$status"
}
trap cleanup EXIT

wait_for() { # seconds, command...
  local seconds=$1
  shift
  for _ in $(seq 1 $((seconds * 10))); do
    "$@" 2>/dev/null && return 0
    sleep 0.1
  done
  echo "timed out waiting for: $*" >&2
  return 1
}

start_server() {
  build/etserver --port "$ET_PORT" --serverfifo="$ET_FIFO" -l "$LOGS/server" \
    --logtostdout >"$LOGS/server-stdout.log" 2>&1 &
  wait_for 10 grep -q "Listening on .*:$ET_PORT/" "$LOGS/server-stdout.log"
}

# Starts `et <args>` under script(1) so the client has a real tty; input comes
# from a held-open fifo on file descriptor <fd>.
start_client() { # fd, log, et args...
  local fd=$1 log=$2
  shift 2
  mkfifo "$RUN_DIR/in$fd"
  eval "exec $fd<>\"$RUN_DIR/in$fd\""
  script -qec "$ET $*" /dev/null <"$RUN_DIR/in$fd" >"$log" 2>&1 &
}

# SIGKILLs a client (and its script wrapper), leaving the remote session.
kill_client() { # et args as passed to start_client
  pkill -KILL -f -- "$ET $*"
  wait_for 10 no_process "$ET $*"
}

no_process() { # pattern
  ! pgrep -f -- "$1" >/dev/null
}

terminal_pids() {
  pgrep -x etterminal -a | grep -F -- "--serverfifo=$ET_FIFO" | cut -d' ' -f1 |
    sort | tr '\n' ' '
}

ssh -o 'PreferredAuthentications=publickey' localhost "echo" || exit 1
ssh -o "StrictHostKeyChecking no" localhost echo "Bypassing host check"
