#include "sentry_boot.h"
#include "sentry_thread_stackwalk.h"

#if defined(SENTRY_PLATFORM_MACOS)

#    include "sentry.h"
#    include "sentry_logger.h"

#    include <mach/mach.h>
#    include <mach/thread_act.h>
#    include <string.h>

size_t
sentry__thread_stackwalk(uint64_t target_tid, void **ips, size_t max)
{
    task_t task = mach_task_self();
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t count = 0;
    if (task_threads(task, &threads, &count) != KERN_SUCCESS) {
        return 0;
    }

    thread_t target = MACH_PORT_NULL;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        thread_identifier_info_data_t id_info;
        mach_msg_type_number_t id_count = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(threads[i], THREAD_IDENTIFIER_INFO,
                (thread_info_t)&id_info, &id_count)
                == KERN_SUCCESS
            && id_info.thread_id == target_tid) {
            if (target != MACH_PORT_NULL) {
                mach_port_deallocate(task, target);
            }
            target = threads[i];
        } else {
            mach_port_deallocate(task, threads[i]);
        }
    }
    vm_deallocate(task, (vm_address_t)threads, count * sizeof(thread_t));

    if (target == MACH_PORT_NULL) {
        return 0;
    }

    size_t n = 0;
    if (thread_suspend(target) == KERN_SUCCESS) {
        // Capture register state into a ucontext and unwind via the existing
        // local unwinder. IPs only; no symbolication while suspended.
        _STRUCT_MCONTEXT mctx;
        memset(&mctx, 0, sizeof(mctx));
        kern_return_t kr;
#    if defined(__aarch64__)
        mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
        kr = thread_get_state(
            target, ARM_THREAD_STATE64, (thread_state_t)&mctx.__ss, &sc);
#    elif defined(__x86_64__)
        mach_msg_type_number_t sc = x86_THREAD_STATE64_COUNT;
        kr = thread_get_state(
            target, x86_THREAD_STATE64, (thread_state_t)&mctx.__ss, &sc);
#    else
        kr = KERN_FAILURE;
#    endif
        if (kr == KERN_SUCCESS) {
            ucontext_t uc;
            memset(&uc, 0, sizeof(uc));
            uc.uc_mcontext = &mctx;
            sentry_ucontext_t s;
            memset(&s, 0, sizeof(s));
            s.user_context = &uc;
            n = sentry_unwind_stack_from_ucontext(&s, ips, max);
        } else {
            SENTRY_DEBUGF("app-hang: thread_get_state failed: %d", kr);
        }
        thread_resume(target); // ALWAYS resume
    }
    mach_port_deallocate(task, target);
    return n;
}

#endif
