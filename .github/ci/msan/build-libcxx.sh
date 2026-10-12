#!/usr/bin/env bash
set -euo pipefail

# The release commit is pinned so a moved tag cannot change the runtime.
llvm_revision=3b5b5c1ec4a3095ab096dd780e84d7ab81f3d7ff # llvmorg-18.1.8
prefix="${MSAN_LIBCXX_ROOT:-${PWD}/.msan-libcxx}"
work="${PWD}/.msan-runtime-build"
compiler="${CXX:-clang++-18}"
recipe=$(cat "$0"; "$compiler" --version)
stamp=$(printf '%s' "$recipe" | sha256sum | cut -d' ' -f1)
if [[ -f "${prefix}/.recipe" && "$(cat "${prefix}/.recipe")" == "$stamp" ]]; then
  exit 0
fi
mkdir -p "$work" "$prefix"
if [[ ! -d "${work}/src/.git" ]]; then
  git init "${work}/src"
  git -C "${work}/src" remote add origin https://github.com/llvm/llvm-project.git
  git -C "${work}/src" sparse-checkout init --cone
  git -C "${work}/src" sparse-checkout set runtimes cmake libcxx libcxxabi llvm/cmake llvm/utils
  git -C "${work}/src" -c protocol.version=2 fetch --depth 1 --filter=blob:none origin "$llvm_revision"
  git -C "${work}/src" checkout --detach FETCH_HEAD
fi
[[ "$(git -C "${work}/src" rev-parse HEAD)" == "$llvm_revision" ]]
# libgcc's exception unwinder is not instrumented. Match LLVM/OSS-Fuzz's
# exclusion for the ABI entry point it calls with raw register state.
printf 'fun:__gxx_personality_*\n' > "${work}/ignorelist.txt"
cmake -S "${work}/src/runtimes" -B "${work}/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER="${CC:-clang-18}" -DCMAKE_CXX_COMPILER="$compiler" \
  -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DLLVM_ENABLE_RUNTIMES='libcxx;libcxxabi' \
  -DLLVM_USE_SANITIZER=MemoryWithOrigins \
  -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF \
  -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF \
  -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
  -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize-ignorelist=${work}/ignorelist.txt"
cmake --build "${work}/build" --parallel "${CI_PARALLEL_LEVEL:-4}"
cmake --install "${work}/build"
printf '%s\n' "$stamp" > "${prefix}/.recipe"
