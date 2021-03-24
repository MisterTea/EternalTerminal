#include "sentry_boot.h"

// XXX: Make into a CMake check
// XXX: IBM i PASE offers libbacktrace in libutil, but not available in AIX
#if defined(SENTRY_PLATFORM_DARWIN) || defined(__GLIBC__) || defined(__PASE__)
#    define HAS_EXECINFO_H
#endif

#ifdef HAS_EXECINFO_H
#    include <execinfo.h>
#endif

size_t
sentry__unwind_stack_libbacktrace(
    void *addr, const sentry_ucontext_t *uctx, void **ptrs, size_t max_frames)
{
    if (addr) {
#if defined(SENTRY_PLATFORM_MACOS) && defined(MAC_OS_X_VERSION_10_14)          \
    && __has_builtin(__builtin_available)
        if (__builtin_available(macOS 10.14, *)) {
            return (size_t)backtrace_from_fp(addr, ptrs, (int)max_frames);
        }
#endif
        return 0;
    } else if (uctx) {
        return 0;
    }
#ifdef HAS_EXECINFO_H
    return (size_t)backtrace(ptrs, (int)max_frames);
#else
    (void)ptrs;
    (void)max_frames;
    return 0;
#endif
}
