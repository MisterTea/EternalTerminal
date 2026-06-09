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

kill -9 $first_server_pid
kill -9 $second_server_pid
