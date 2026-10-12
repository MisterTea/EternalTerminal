#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
: "${MSAN_LIBCXX_ROOT:?Instrumented libc++ is required}"
mkdir -p "${root}/.msan-smoke"
"${CXX:-clang++-18}" -g -O0 -fsanitize=memory -fsanitize-memory-track-origins=2 \
  -fno-omit-frame-pointer -fPIE -pie -stdlib=libc++ -nostdinc++ \
  -isystem "${MSAN_LIBCXX_ROOT}/include/c++/v1" -L"${MSAN_LIBCXX_ROOT}/lib" \
  "${root}/.github/ci/msan/smoke.cpp" -o "${root}/.msan-smoke/check"
"${root}/.msan-smoke/check"
set +e
MSAN_OPTIONS=exit_code=86 "${root}/.msan-smoke/check" uninitialized > "${root}/.msan-smoke/diagnostic.log" 2>&1
status=$?
set -e
[[ "$status" == 86 ]]
grep -q 'MemorySanitizer: use-of-uninitialized-value' "${root}/.msan-smoke/diagnostic.log"
echo 'MSan runtime detects uninitialized memory and accepts instrumented libc++'
