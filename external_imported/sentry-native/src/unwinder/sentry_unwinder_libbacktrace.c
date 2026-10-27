#include "sentry_boot.h"

// XXX: Make into a CMake check
// XXX: IBM i PASE offers libbacktrace in libutil, but not available in AIX
#if defined(__GLIBC__) || defined(__PASE__)
#    define HAS_EXECINFO_H
#endif

#ifdef HAS_EXECINFO_H
#    include <execinfo.h>
#endif

size_t
sentry__unwind_stack_libbacktrace(
    void *addr, const sentry_ucontext_t *uctx, void **ptrs, size_t max_frames)
{
    if (addr || uctx) {
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
