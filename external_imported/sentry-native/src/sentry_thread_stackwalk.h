#ifndef SENTRY_THREAD_STACKWALK_H_INCLUDED
#define SENTRY_THREAD_STACKWALK_H_INCLUDED

#include "sentry_boot.h"

#include <stddef.h>
#include <stdint.h>

// A platform thread stackwalker is compiled in only for the targets below.
// On any other target `sentry__thread_stackwalk` has no definition.
#if defined(SENTRY_PLATFORM_MACOS) || defined(SENTRY_PLATFORM_WINDOWS)         \
    || defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
#    define SENTRY_HAS_THREAD_STACKWALK 1
#else
#    define SENTRY_HAS_THREAD_STACKWALK 0
#endif

#if SENTRY_HAS_THREAD_STACKWALK
// Captures the call stack of another thread by suspending it, walking its
// stack, and writing the instruction pointers into `ips_out` (IPs only; no
// symbolication while suspended). Returns the number of frames captured.
size_t sentry__thread_stackwalk(
    uint64_t target_tid, void **ips_out, size_t max_frames);
#endif

#endif
