#include "sentry_boot.h"

#define DEFINE_UNWINDER(Func)                                                  \
    size_t sentry__unwind_stack_##Func(void *addr,                             \
        const sentry_ucontext_t *uctx, void **ptrs, size_t max_frames)

#define TRY_UNWINDER(Func)                                                     \
    do {                                                                       \
        size_t rv = sentry__unwind_stack_##Func(addr, uctx, ptrs, max_frames); \
        if (rv > 0) {                                                          \
            return rv;                                                         \
        }                                                                      \
    } while (0)

DEFINE_UNWINDER(libunwindstack);
DEFINE_UNWINDER(libbacktrace);
DEFINE_UNWINDER(dbghelp);

static size_t
unwind_stack(
    void *addr, const sentry_ucontext_t *uctx, void **ptrs, size_t max_frames)
{
#ifdef SENTRY_WITH_UNWINDER_LIBUNWINDSTACK
    TRY_UNWINDER(libunwindstack);
#endif
#ifdef SENTRY_WITH_UNWINDER_LIBBACKTRACE
    TRY_UNWINDER(libbacktrace);
#endif
#ifdef SENTRY_WITH_UNWINDER_DBGHELP
    TRY_UNWINDER(dbghelp);
#endif
    return 0;
}

size_t
sentry_unwind_stack(void *addr, void **stacktrace_out, size_t max_len)
{
    return unwind_stack(addr, NULL, stacktrace_out, max_len);
}

size_t
sentry_unwind_stack_from_ucontext(
    const sentry_ucontext_t *uctx, void **stacktrace_out, size_t max_len)
{
    return unwind_stack(NULL, uctx, stacktrace_out, max_len);
}
