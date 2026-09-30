#!/bin/bash
# Drives et, etserver, and etterminal into setup failures and requires each to
# exit with status 1 and a readable message: not abort (134), crash (139),
# hang (124, from timeout), or report success.
#
# ET_BUILD_DIR selects the binaries (default: build). Cases that bootstrap a
# session need passwordless `ssh localhost`; they are skipped when it is not
# available, except under CI where they are required.
set -u
export ET_NO_TELEMETRY=YES
cd "$(dirname "$0")/../.."

BUILD_DIR=$(cd "${ET_BUILD_DIR:-build}" && pwd)
ET_BIN=$BUILD_DIR/et
ETSERVER_BIN=$BUILD_DIR/etserver
ETTERMINAL_BIN=$BUILD_DIR/etterminal
TIMEOUT=$(command -v timeout || command -v gtimeout || true)

# Like timeout(1): exit status 124 when the command is killed for running long.
run_with_timeout() { # seconds, command...
  local seconds=$1
  shift
  if [ -n "$TIMEOUT" ]; then
    "$TIMEOUT" "$seconds" "$@"
    return
  fi
  "$@" &
  local pid=$!
  (sleep "$seconds" && kill -KILL "$pid" 2>/dev/null) &
  local watchdog=$!
  local status=0
  wait "$pid" || status=$?
  if kill "$watchdog" 2>/dev/null; then
    wait "$watchdog" 2>/dev/null
    return "$status"
  fi
  return 124
}

RUN_DIR=$(mktemp -d "${TMPDIR:-/tmp}/et_setup_failures.XXXXXXXX")
LOGS=$RUN_DIR/logs
ET_FIFO=$RUN_DIR/etserver.fifo
export HOME=$RUN_DIR/home
mkdir -p "$HOME" "$LOGS"
IDPASSKEY=abcdefghijklmnop/0123456789abcdef0123456789abcdef
failures=()
background_pids=()

cleanup() {
  for pid in "${background_pids[@]}"; do
    kill -KILL "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
  done
  pkill -KILL -f -- "--serverfifo=$ET_FIFO" 2>/dev/null
  rm -rf -- "$RUN_DIR"
}
trap cleanup EXIT

# Picks a loopback port nothing is listening on.
free_port() {
  python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'
}

# check_failure NAME EXPECTED_TEXT COMMAND...
# Sets $problem to why COMMAND did not fail cleanly (empty when it exited 1
# and printed EXPECTED_TEXT), and $log to its output file.
check_failure() {
  local name=$1 expected=$2
  shift 2
  log=$LOGS/$name.log
  local status=0
  run_with_timeout 60 "$@" </dev/null >"$log" 2>&1 || status=$?
  problem=""
  if [ "$status" -eq 124 ]; then
    problem="timed out (hang)"
  elif [ "$status" -ge 128 ]; then
    problem="killed by signal $((status - 128)) (exit status $status)"
  elif [ "$status" -ne 1 ]; then
    problem="exit status $status, expected 1"
  elif ! grep -qF -- "$expected" "$log"; then
    problem="output does not contain '$expected'"
  fi
}

# expect_failure NAME EXPECTED_TEXT COMMAND...
expect_failure() {
  check_failure "$@"
  if [ -n "$problem" ]; then
    echo "FAIL $1: $problem"
    sed 's/^/    /' "$log" | tail -n 20
    failures+=("$1")
  else
    echo "ok   $1"
  fi
}

have_ssh() {
  ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
    -o PreferredAuthentications=publickey localhost true >/dev/null 2>&1
}

wait_for_log() { # seconds, file, text
  for _ in $(seq 1 $(($1 * 10))); do
    grep -qF -- "$3" "$2" 2>/dev/null && return 0
    sleep 0.1
  done
  return 1
}

# --- etserver ---------------------------------------------------------------

printf '[Networking]\nport=abc\n' >"$RUN_DIR/bad-port.cfg"
expect_failure etserver_bad_config_port "Invalid integer for [Networking] port" \
  "$ETSERVER_BIN" --cfgfile "$RUN_DIR/bad-port.cfg" --logtostdout \
  --serverfifo="$RUN_DIR/unused.fifo"

expect_failure etserver_missing_config "Invalid config file" \
  "$ETSERVER_BIN" --cfgfile "$RUN_DIR/missing.cfg" --logtostdout \
  --serverfifo="$RUN_DIR/unused.fifo"

expect_failure etserver_negative_disconnect_timeout "non-negative" \
  "$ETSERVER_BIN" --disconnect-timeout -1 --logtostdout \
  --serverfifo="$RUN_DIR/unused.fifo"

expect_failure etserver_unresolvable_bindip "Failed to resolve address" \
  "$ETSERVER_BIN" --port "$(free_port)" --bindip not-a-host.invalid \
  --logtostdout --serverfifo="$RUN_DIR/unused.fifo"

ET_PORT=$(free_port)
"$ETSERVER_BIN" --port "$ET_PORT" --serverfifo="$ET_FIFO" -l "$LOGS" \
  --logtostdout >"$LOGS/etserver.out" 2>&1 &
background_pids+=($!)
if ! wait_for_log 10 "$LOGS/etserver.out" "Listening on"; then
  echo "etserver did not start" >&2
  cat "$LOGS/etserver.out" >&2
  exit 1
fi

expect_failure etserver_port_in_use "Could not bind to any interface" \
  "$ETSERVER_BIN" --port "$ET_PORT" --logtostdout \
  --serverfifo="$RUN_DIR/second.fifo"

# --- etterminal -------------------------------------------------------------

expect_failure etterminal_no_router "Error connecting to router" \
  "$ETTERMINAL_BIN" --idpasskey "$IDPASSKEY" --serverfifo="$RUN_DIR/no-router" \
  --logdir "$LOGS"

expect_failure etterminal_jump_no_router "communicating with et daemon" \
  "$ETTERMINAL_BIN" --jump --dsthost 127.0.0.1 --dstport "$ET_PORT" \
  --idpasskey "$IDPASSKEY" --serverfifo="$RUN_DIR/no-router" --logdir "$LOGS"

expect_failure etterminal_malformed_idpasskey "Invalid idpasskey" \
  "$ETTERMINAL_BIN" --idpasskey no-separator --serverfifo="$ET_FIFO" \
  --logdir "$LOGS"

# --- et, before any connection ----------------------------------------------

expect_failure et_overflowing_host_port "Invalid host positional arg" \
  "$ET_BIN" --logtostdout --no-persist "localhost:99999999999999999999"

expect_failure et_unreachable_server "Could not reach the ET server" \
  "$ET_BIN" --logtostdout --no-persist --no-ssh-config \
  --port "$(free_port)" 127.0.0.1

# --- et, after the ssh bootstrap succeeds -----------------------------------

if have_ssh; then
  cat >"$RUN_DIR/etterminal" <<EOF
#!/bin/sh
exec "$ETTERMINAL_BIN" --logdir="$LOGS" "\$@"
EOF
  chmod 700 "$RUN_DIR/etterminal"
  ET=("$ET_BIN" --serverfifo="$ET_FIFO" --terminal-path "$RUN_DIR/etterminal"
    --logtostdout --no-persist --no-terminal)

  expect_failure et_ssh_style_tunnel_bad_port "Invalid tunnel argument" \
    "${ET[@]}" -L localhost:notaport:localhost:22 "localhost:$ET_PORT"

  expect_failure et_ssh_style_tunnel_overflow "Invalid tunnel argument" \
    "${ET[@]}" -L 99999999999999999999:localhost:22 "localhost:$ET_PORT"

  expect_failure et_agent_forward_without_agent "SSH_AUTH_SOCK" \
    env -u SSH_AUTH_SOCK "${ET[@]}" --forward-ssh-agent "localhost:$ET_PORT"

  # ssh bootstraps a real etterminal, but the ET port accepts and drops every
  # connection, so the client must give up instead of spinning in writePacket.
  python3 - "$LOGS/dropper.port" <<'PY' &
import socket, sys
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0))
s.listen(16)
with open(sys.argv[1], "w") as f:
    f.write(str(s.getsockname()[1]))
while True:
    conn, _ = s.accept()
    conn.close()
PY
  background_pids+=($!)
  wait_for_log 10 "$LOGS/dropper.port" "" && sleep 0.2
  DROP_PORT=$(cat "$LOGS/dropper.port")
  expect_failure et_initial_connection_dropped "Could not make initial connection" \
    "${ET[@]}" "localhost:$DROP_PORT"
elif [ -n "${CI:-}" ]; then
  echo "FAIL ssh: passwordless ssh localhost is required under CI"
  failures+=(ssh)
else
  echo "skip ssh-bootstrap cases: passwordless ssh localhost is unavailable"
fi

if [ "${#failures[@]}" -ne 0 ]; then
  echo "setup_failures.sh: ${#failures[@]} failed: ${failures[*]}"
  exit 1
fi
echo "setup_failures.sh: OK"
