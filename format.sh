#!/bin/bash

if command -v clang-format-18 >/dev/null 2>&1; then
	formatter=(clang-format-18)
elif command -v clang-format >/dev/null 2>&1; then
	formatter=(clang-format)
else
	formatter=(nix run 'github:NixOS/nixpkgs/nixos-unstable#llvmPackages_18.clang-tools' -- clang-format)
fi

find ./src ./test -type f | grep "\.[hc]pp" | xargs "${formatter[@]}" --style=Google -i
