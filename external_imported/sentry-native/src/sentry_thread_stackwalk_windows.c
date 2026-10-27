#include "sentry_thread_stackwalk.h"

#include "sentry_boot.h"

#if defined(SENTRY_PLATFORM_WINDOWS)

#    include "sentry.h"
#    include "sentry_logger.h"

#    include <string.h>
#    include <windows.h>

size_t
sentry__thread_stackwalk(uint64_t target_tid, void **ips, size_t max)
{
    HANDLE h = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
        FALSE, (DWORD)target_tid);
    if (!h) {
        return 0;
    }

    size_t n = 0;
    if (SuspendThread(h) != (DWORD)-1) {
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(h, &ctx)) { // forces the suspend to complete
            EXCEPTION_RECORD er;
            memset(&er, 0, sizeof(er));
            EXCEPTION_POINTERS ep;
            ep.ExceptionRecord = &er;
            ep.ContextRecord = &ctx;
            sentry_ucontext_t s;
            memset(&s, 0, sizeof(s));
            s.exception_ptrs = ep;
            // NOTE: the shared dbghelp unwinder drives StackWalk64 with
            // GetCurrentThread() (the watchdog's pseudo-handle), not the
            // suspended target thread. On x64 the walk is CONTEXT-driven and
            // reads the target's stack from the shared process address space,
            // so the captured IPs are correct. On x86 (frame-pointer mode) and
            // ARM64, StackWalk64 may use the thread handle for register
            // updates, so cross-thread unwinding there needs the target handle
            // threaded through the unwinder. Tracked as a follow-up; x64 is the
            // validated path for this experimental feature.
            n = sentry_unwind_stack_from_ucontext(&s, ips, max);
        } else {
            SENTRY_DEBUGF(
                "app-hang: GetThreadContext failed: %lu", GetLastError());
        }
        ResumeThread(h); // ALWAYS resume
    }
    CloseHandle(h);
    return n;
}

#endif
