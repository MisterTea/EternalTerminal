// Copyright 2006-2008 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_IMMEDIATE_CRASH_H_
#define MINI_CHROMIUM_BASE_IMMEDIATE_CRASH_H_

#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <intrin.h>
#endif  // BUILDFLAG(IS_WIN)

#if defined(COMPILER_GCC)
#define IMMEDIATE_CRASH_ALWAYS_INLINE inline __attribute__((__always_inline__))
#elif defined(COMPILER_MSVC)
#define IMMEDIATE_CRASH_ALWAYS_INLINE __forceinline
#else
#define IMMEDIATE_CRASH_ALWAYS_INLINE inline
#endif

namespace base {

[[noreturn]] IMMEDIATE_CRASH_ALWAYS_INLINE void ImmediateCrash() {
#if defined(COMPILER_MSVC)
  __debugbreak();
#if defined(ARCH_CPU_X86_FAMILY)
  __ud2();
#elif defined(ARCH_CPU_ARM64)
  __hlt(0);
#else
#error Unsupported Windows Arch
#endif
#elif defined(ARCH_CPU_X86_FAMILY)
  asm("int3; ud2;");
#elif defined(ARCH_CPU_ARMEL)
  asm("bkpt #0; udf #0;");
#elif defined(ARCH_CPU_ARM64)
  asm("brk #0; hlt #0;");
#else
  __builtin_trap();
#endif
#if defined(__clang__) || defined(COMPILER_GCC)
  __builtin_unreachable();
#endif  // defined(__clang__) || defined(COMPILER_GCC)
}

}  // namespace base

#endif  // MINI_CHROMIUM_BASE_IMMEDIATE_CRASH_H_
