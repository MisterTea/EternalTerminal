#!/bin/bash

# CI uses clang-format 18; prefer that over a newer PATH clang-format so local
# format.sh matches the Formatting Check workflow.
if command -v clang-format-18 >/dev/null 2>&1; then
	formatter=(clang-format-18)
elif [[ -x /opt/homebrew/opt/llvm@18/bin/clang-format ]]; then
	formatter=(/opt/homebrew/opt/llvm@18/bin/clang-format)
elif [[ -x /usr/local/opt/llvm@18/bin/clang-format ]]; then
	formatter=(/usr/local/opt/llvm@18/bin/clang-format)
elif command -v clang-format >/dev/null 2>&1; then
	formatter=(clang-format)
else
	formatter=(nix run 'github:NixOS/nixpkgs/nixos-unstable#llvmPackages_18.clang-tools' -- clang-format)
fi

find ./src ./test -type f | grep "\.[hc]pp" | xargs "${formatter[@]}" --style=Google -i
