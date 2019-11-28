#!/usr/bin/env bash

pkg install cmake ninja protobuf libsodium
git clone --branch release git@github.com:MisterTea/EternalTerminal.git
mkdir -p EternalTerminal/build
cd EternalTerminal/build
cmake ..
make -j`nproc`
