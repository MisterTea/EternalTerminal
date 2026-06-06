#!/bin/bash
set -euo pipefail
set -x

ROOT=$(git rev-parse --show-toplevel)
CURRENT_BUILD="${ET_BUILD_DIR:-$ROOT/build}"
OLD_TAG="${ET_COMPAT_TAG:-et-v6.2.10}"
OLD_ROOT="${ET_COMPAT_OLD_ROOT:-$ROOT/build/compat/$OLD_TAG}"
OLD_BUILD="$OLD_ROOT/build"

OLD_FIFO="/tmp/etserver.compat.old.idpasskey.fifo"
CURRENT_FIFO="/tmp/etserver.compat.current.idpasskey.fifo"
OLD_LOG_DIR="/tmp/et_test_logs/backwards_compatibility/old"
CURRENT_LOG_DIR="/tmp/et_test_logs/backwards_compatibility/current"
OLD_SERVER_PID=""
CURRENT_SERVER_PID=""

cleanup() {
  if [ -n "$OLD_SERVER_PID" ]; then
    kill -9 "$OLD_SERVER_PID" 2>/dev/null || true
  fi
  if [ -n "$CURRENT_SERVER_PID" ]; then
    kill -9 "$CURRENT_SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

run_and_expect() {
  local expected="$1"
  shift

  local output
  output=$(mktemp)

  set +e
  "$@" 2>&1 | tee "$output"
  local command_status=${PIPESTATUS[0]}
  set -e

  if [ "$command_status" -eq 0 ] && grep -q "$expected" "$output"; then
    rm -f "$output"
    return 0
  fi

  echo "Command failed or expected output was not found: $expected"
  echo "Command status: $command_status"
  cat "$output"
  rm -f "$output"
  return 1
}

require_binary() {
  if [ ! -x "$1" ]; then
    echo "Missing executable: $1"
    exit 1
  fi
}

require_binary "$CURRENT_BUILD/et"
require_binary "$CURRENT_BUILD/etserver"
require_binary "$CURRENT_BUILD/etterminal"

if ! git -C "$ROOT" rev-parse -q --verify "refs/tags/$OLD_TAG^{commit}" >/dev/null; then
  git -C "$ROOT" fetch --depth=1 origin "refs/tags/$OLD_TAG:refs/tags/$OLD_TAG"
fi

OLD_COMMIT=$(git -C "$ROOT" rev-parse "$OLD_TAG^{commit}")
if [ ! -d "$OLD_ROOT/.git" ]; then
  rm -rf "$OLD_ROOT"
  mkdir -p "$(dirname "$OLD_ROOT")"
  git clone "$ROOT" "$OLD_ROOT"
fi

git -C "$OLD_ROOT" fetch --tags "$ROOT"
git -C "$OLD_ROOT" checkout --force "$OLD_COMMIT"

rm -rf "$OLD_BUILD"
mkdir -p "$OLD_BUILD"
cmake -S "$OLD_ROOT" -B "$OLD_BUILD" -GNinja \
  -DDISABLE_VCPKG=ON \
  -DDISABLE_TELEMETRY=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$OLD_BUILD" --target et etserver etterminal --parallel "$(nproc)"

require_binary "$OLD_BUILD/et"
require_binary "$OLD_BUILD/etserver"
require_binary "$OLD_BUILD/etterminal"

rm -f "$OLD_FIFO" "$CURRENT_FIFO"
mkdir -p "$OLD_LOG_DIR" "$CURRENT_LOG_DIR"

export ET_NO_TELEMETRY=YES
if [ -z "${TERM:-}" ] || [ "$TERM" = "dumb" ]; then
  export TERM=xterm-256color
fi

SSH_PORT=$(ssh -G localhost 2>/dev/null | awk '/^port /{print $2}')
SSH_PORT=${SSH_PORT:-22}
SSH_OPTIONS=("--ssh-option" "Port=$SSH_PORT" "--ssh-option" "StrictHostKeyChecking=no")

"$OLD_BUILD/etserver" --port 9920 --serverfifo="$OLD_FIFO" -l "$OLD_LOG_DIR" &
OLD_SERVER_PID=$!
sleep 3

run_and_expect "compat new to old" \
  "$CURRENT_BUILD/et" -c "echo 'compat new to old'" \
  --serverfifo="$OLD_FIFO" \
  --terminal-path "$OLD_BUILD/etterminal" \
  --logtostdout \
  localhost:9920

run_and_expect "compat new to old ipv6 full" \
  "$CURRENT_BUILD/et" -c "echo 'compat new to old ipv6 full'" \
  --serverfifo="$OLD_FIFO" \
  --terminal-path "$OLD_BUILD/etterminal" \
  --logtostdout \
  "${SSH_OPTIONS[@]}" \
  --port 9920 \
  0:0:0:0:0:0:0:1

run_and_expect "compat new to old ipv6 full host port" \
  "$CURRENT_BUILD/et" -c "echo 'compat new to old ipv6 full host port'" \
  --serverfifo="$OLD_FIFO" \
  --terminal-path "$OLD_BUILD/etterminal" \
  --logtostdout \
  "${SSH_OPTIONS[@]}" \
  0:0:0:0:0:0:0:1:9920

run_and_expect "compat new to old ipv6 abbreviated" \
  "$CURRENT_BUILD/et" -c "echo 'compat new to old ipv6 abbreviated'" \
  --serverfifo="$OLD_FIFO" \
  --terminal-path "$OLD_BUILD/etterminal" \
  --logtostdout \
  "${SSH_OPTIONS[@]}" \
  --port 9920 \
  ::1

kill -9 "$OLD_SERVER_PID" 2>/dev/null || true
OLD_SERVER_PID=""

"$CURRENT_BUILD/etserver" --port 9930 --serverfifo="$CURRENT_FIFO" -l "$CURRENT_LOG_DIR" &
CURRENT_SERVER_PID=$!
sleep 3

run_and_expect "compat old to new" \
  "$OLD_BUILD/et" -c "echo 'compat old to new'" \
  --serverfifo="$CURRENT_FIFO" \
  --terminal-path "$CURRENT_BUILD/etterminal" \
  --logtostdout \
  localhost:9930
