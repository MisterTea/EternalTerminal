#!/bin/bash
set -x
set -e

ssh -o 'PreferredAuthentications=publickey' localhost "echo" || exit 1 # Fails if we can't ssh into localhost without a password

# Bypass host check
ssh -o "StrictHostKeyChecking no" localhost echo "Bypassing host check 1"
ssh -o "StrictHostKeyChecking no" 127.0.0.1 echo "Bypassing host check 2"

mkdir -p /tmp/et_test_logs/connect_with_jumphost/1
build/etserver --port 9900  --serverfifo=/tmp/etserver.idpasskey.fifo1 -l /tmp/et_test_logs/connect_with_jumphost/1 &
first_server_pid=$!

mkdir -p /tmp/et_test_logs/connect_with_jumphost/2
build/etserver --port 9901 --serverfifo=/tmp/etserver.idpasskey.fifo2 -l /tmp/et_test_logs/connect_with_jumphost/2 &
second_server_pid=$!
sleep 3

# Make sure servers are working
build/et -c "echo 'Hello World 1!'" --serverfifo=/tmp/etserver.idpasskey.fifo1 --logtostdout --terminal-path $PWD/build/etterminal localhost:9900
build/et -c "echo 'Hello World 2!'" --serverfifo=/tmp/etserver.idpasskey.fifo2 --logtostdout --terminal-path $PWD/build/etterminal localhost:9901

build/et -c "echo 'Hello World 3!'" --serverfifo=/tmp/etserver.idpasskey.fifo2 --logtostdout --terminal-path $PWD/build/etterminal --jumphost localhost --jport 9900 --jserverfifo=/tmp/etserver.idpasskey.fifo1 127.0.0.1:9901 # We can't use 'localhost' for both the jumphost and the destination because ssh doesn't support keeping them the same.

# Test SSH config ProxyJump with alias and port specification
# Backup existing SSH config if present, or track that we created it
mkdir -p ~/.ssh
SSH_CONFIG_BACKUP=""
SSH_CONFIG_CREATED=false
if [ -f ~/.ssh/config ]; then
  SSH_CONFIG_BACKUP=$(mktemp)
  cp ~/.ssh/config "$SSH_CONFIG_BACKUP"
else
  SSH_CONFIG_CREATED=true
fi

# Cleanup function to restore SSH config on exit (including failures)
cleanup_ssh_config() {
  if [ -n "$SSH_CONFIG_BACKUP" ]; then
    mv "$SSH_CONFIG_BACKUP" ~/.ssh/config
  elif [ "$SSH_CONFIG_CREATED" = true ]; then
    rm -f ~/.ssh/config
  fi
}
trap cleanup_ssh_config EXIT

# Append test configuration to SSH config
cat >> ~/.ssh/config <<EOF

# Temporary ET test configuration
Host et_test_jumphost_alias
    HostName localhost
    Port 22
    User $USER

Host et_test_destination_with_port
    HostName 127.0.0.1
    ProxyJump et_test_jumphost_alias:22
    User $USER
EOF

# Test 4: ProxyJump with alias resolution and explicit SSH port
# This tests that the alias "et_test_jumphost_alias:22" is resolved to "localhost:22"
# and that the port specification is preserved in the ssh -J command
echo "Test 4: ProxyJump with alias and explicit SSH port (testing alias resolution + port preservation)"
TEST4_OUTPUT=$(mktemp)
if build/et -c "echo 'Hello World 4!'" --serverfifo=/tmp/etserver.idpasskey.fifo2 --logtostdout --terminal-path $PWD/build/etterminal --jport 9900 --jserverfifo=/tmp/etserver.idpasskey.fifo1 et_test_destination_with_port 2>&1 | tee "$TEST4_OUTPUT" | grep -q "Hello World 4!"; then
  # Verify alias was resolved in logs
  if grep -q "Resolved jumphost alias 'et_test_jumphost_alias' to hostname: localhost" "$TEST4_OUTPUT"; then
    echo "Test 4 passed: Alias resolution confirmed"
  else
    echo "Test 4 failed: Connection succeeded but alias resolution log not found"
    cat "$TEST4_OUTPUT"
    rm -f "$TEST4_OUTPUT"
    exit 1
  fi
else
  echo "Test 4 failed: Connection or output verification failed"
  cat "$TEST4_OUTPUT"
  rm -f "$TEST4_OUTPUT"
  exit 1
fi
rm -f "$TEST4_OUTPUT"

kill -9 $first_server_pid
kill -9 $second_server_pid
