#include "sentry_boot.h"
#include "sentry_logger.h"
#define UNW_LOCAL_ONLY
#include <libunwind.h>

size_t
sentry__unwind_stack_libunwind(
    void *addr, const sentry_ucontext_t *uctx, void **ptrs, size_t max_frames)
{
    if (addr) {
        return 0;
    }

    unw_cursor_t cursor;
    if (uctx) {
        int ret = unw_init_local2(&cursor, (unw_context_t *)uctx->user_context,
            UNW_INIT_SIGNAL_FRAME);
        if (ret != 0) {
            SENTRY_WARN("Failed to initialize libunwind with ucontext");
            return 0;
        }
    } else {
        unw_context_t uc;
        int ret = unw_getcontext(&uc);
        if (ret != 0) {
            SENTRY_WARN("Failed to retrieve context with libunwind");
            return 0;
        }

        ret = unw_init_local(&cursor, &uc);
        if (ret != 0) {
            SENTRY_WARN("Failed to initialize libunwind with local context");
            return 0;
        }
    }

    size_t frame_idx = 0;
    while (unw_step(&cursor) > 0 && frame_idx < max_frames - 1) {
        unw_word_t ip = 0;
        unw_get_reg(&cursor, UNW_REG_IP, &ip);
        ptrs[frame_idx] = (void *)ip;
        unw_word_t sp = 0;
        unw_get_reg(&cursor, UNW_REG_SP, &sp);
        frame_idx++;
    }
    return frame_idx + 1;
}
