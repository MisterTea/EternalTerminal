#!/bin/bash
# shellcheck disable=SC2016 # $-expressions expand in the remote shell
# A saved session survives its client dying and can be reattached or killed.
ET_PORT=9940
# shellcheck source=test/system_tests/session_test_lib.sh
source "$(dirname "$0")/session_test_lib.sh"
start_server

# Ordinary sessions are saved under a generated name; --no-persist opts out.
start_client 8 "$LOGS/unnamed.log" -N "localhost:$ET_PORT"
wait_for 30 grep -q 'ET running' "$LOGS/unnamed.log"
build/et --list | grep -E -q '^20[0-9]{6}-[[:alnum:]]{4}'
start_client 9 "$LOGS/nopersist.log" --no-persist -N "localhost:$ET_PORT"
wait_for 30 grep -q 'ET running' "$LOGS/nopersist.log"
[ "$(find "$SESSIONS" -type f | wc -l)" -eq 1 ]

# Pipe (-T) sessions are never saved or reattachable, and command stdout
# arrives byte-exact (the client adds its own status lines around it).
PIPE_ET="build/et --serverfifo=$ET_FIFO --terminal-path $RUN_DIR/etterminal"
for flag in "--name pipe localhost:$ET_PORT" "--attach pipe"; do
  # shellcheck disable=SC2086
  $PIPE_ET -T -c true $flag >"$LOGS/pipe-reject.log" 2>&1 </dev/null && exit 1
  grep -q 'no-pty sessions are not saved' "$LOGS/pipe-reject.log"
done
head -c 3000000 /dev/urandom >"$RUN_DIR/blob"
$PIPE_ET -T -c "cat $RUN_DIR/blob" "localhost:$ET_PORT" </dev/null \
  >"$RUN_DIR/blob.out"
perl -0777 -e 'local $/; open my $a, "<:raw", $ARGV[0] or die;
  open my $b, "<:raw", $ARGV[1] or die; exit(index(<$b>, <$a>) < 0)' \
  "$RUN_DIR/blob" "$RUN_DIR/blob.out"
$PIPE_ET -T -c 'seq 1 100000 | md5sum' "localhost:$ET_PORT" </dev/null |
  grep -qxF -- "$(seq 1 100000 | md5sum)"
[ "$(find "$SESSIONS" -type f | wc -l)" -eq 1 ]

# A named session records its OSC title and never prints its credentials.
start_client 10 "$LOGS/alpha.log" --name alpha "localhost:$ET_PORT"
wait_for 30 test -f "$SESSIONS/alpha"
printf 'SENTINEL=abc123; echo PRE-$((6*7))\n' >&10
wait_for 30 grep -q PRE-42 "$LOGS/alpha.log"
printf 'printf "\\033]2;ET_TITLE_TEST\\007"\n' >&10
wait_for 30 grep -q '^title=ET_TITLE_TEST$' "$SESSIONS/alpha"
build/et --list | grep -E -q 'alpha.*ET_TITLE_TEST.*now'
passkey=$(sed -n 's/^passkey=//p' "$SESSIONS/alpha")
grep -qF -- "$passkey" "$LOGS/alpha.log" && exit 1

# Client restart: the record survives and --attach (by title substring)
# resumes the same shell through the reset handshake.
kill_client --name alpha
[ -f "$SESSIONS/alpha" ]
start_client 11 "$LOGS/attach.log" --attach TITLE_TEST
printf 'echo POST-$SENTINEL\n' >&11
wait_for 30 grep -q POST-abc123 "$LOGS/attach.log"

# Ending the shell removes the record.
printf 'exit\n' >&11
wait_for 10 test ! -f "$SESSIONS/alpha"

# --kill ends a session whose client is still attached; a restored copy of
# the ended record is then removed as stale.
start_client 12 "$LOGS/beta.log" --name beta "localhost:$ET_PORT"
wait_for 30 test -f "$SESSIONS/beta"
cp -p "$SESSIONS/beta" "$RUN_DIR/beta.stale"
build/et --kill beta | grep -F -q "Killed session 'beta'"
[ ! -f "$SESSIONS/beta" ]
build/et --attach beta >"$LOGS/killed.log" 2>&1 && exit 1
grep -F -q "No saved session named 'beta'" "$LOGS/killed.log"
cp -p "$RUN_DIR/beta.stale" "$SESSIONS/beta"
build/et --kill beta | grep -F -q "was already gone; removed stale record"
[ ! -f "$SESSIONS/beta" ]

echo "named_sessions.sh: OK"
