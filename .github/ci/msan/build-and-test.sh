#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
# The poller regression exercises descriptors above select()'s 1024 ceiling.
ulimit -n 4096
case $(uname -m) in
  x86_64) architecture=x64 ;;
  aarch64) architecture=arm64 ;;
  *) echo 'MSan CI supports 64-bit Linux x86 and ARM' >&2; exit 1 ;;
esac
export CC=clang-18 CXX=clang++-18
export MSAN_LIBCXX_ROOT="${MSAN_LIBCXX_ROOT:-${root}/.msan-libcxx}"
export MSAN_OPTIONS=halt_on_error=1:exit_code=86:track_origins=2:symbolize=1
export MSAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-18
export VCPKG_DEFAULT_BINARY_CACHE="${VCPKG_DEFAULT_BINARY_CACHE:-${root}/.vcpkg-binary-cache}"
export VCPKG_BINARY_SOURCES="clear;files,${VCPKG_DEFAULT_BINARY_CACHE},readwrite"
mkdir -p "$VCPKG_DEFAULT_BINARY_CACHE"
python3 .github/ci/msan/prepare-ports.py
# Reapply the toolchain's initial flags when a local build is reconfigured.
cmake -S "$root" -B "${root}/build-msan" -G Ninja \
  -UCMAKE_C_FLAGS -UCMAKE_CXX_FLAGS -UCMAKE_EXE_LINKER_FLAGS -UCMAKE_SHARED_LINKER_FLAGS -UUTEMPTER_INCLUDE_DIR -UUTEMPTER_LIBRARIES \
  -DDISABLE_TELEMETRY=ON -DDISABLE_CRASH_LOG=ON -DSANITIZE_MEMORY=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS_RELWITHDEBINFO='-O1 -g -DNDEBUG' \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO='-O1 -g -DNDEBUG' \
  -DCMAKE_DISABLE_FIND_PACKAGE_Unwind=ON \
  -DVCPKG_TARGET_TRIPLET="${architecture}-linux-msan" \
  -DVCPKG_HOST_TRIPLET="${architecture}-linux" \
  -DVCPKG_OVERLAY_TRIPLETS="${root}/.github/ci/msan/triplets" \
  -DVCPKG_OVERLAY_PORTS="${root}/.msan-ports" \
  -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="${root}/.github/ci/msan/toolchain.cmake" \
  -DVCPKG_MANIFEST_DIR="${root}/.github/ci/msan/manifest"
# Do not silently bind PTY tests to an uninstrumented system library.
grep -F "UTEMPTER_LIBRARIES:FILEPATH=${root}/build-msan/vcpkg_installed/${architecture}-linux-msan/lib/libutempter.a" \
  "${root}/build-msan/CMakeCache.txt" >/dev/null
cmake --build "${root}/build-msan" --parallel "${CI_PARALLEL_LEVEL:-4}"
ctest --test-dir "${root}/build-msan" --parallel "${CI_PARALLEL_LEVEL:-4}" \
  --timeout 300 --output-on-failure
