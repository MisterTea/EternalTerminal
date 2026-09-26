#!/usr/bin/env bash
# Idempotent repository bootstrap for the Cloud Agent environment.
# Fetches submodules and performs an incremental build of Eternal Terminal
# using the system-dependency path (no vcpkg). Safe to run repeatedly.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

echo "==> Syncing git submodules"
git submodule sync --recursive
git submodule update --init --recursive

echo "==> Configuring build (system libraries, Ninja)"
cmake \
    -S "${REPO_ROOT}" \
    -B "${REPO_ROOT}/build" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DDISABLE_VCPKG=ON \
    -DDISABLE_TELEMETRY=ON

echo "==> Building all targets"
cmake --build "${REPO_ROOT}/build" --parallel "$(nproc)"

echo "==> Build complete. Binaries in ${REPO_ROOT}/build:"
ls -1 "${REPO_ROOT}/build" | grep -E '^(et|etserver|etterminal|htm|htmd|et-test)$' || true
