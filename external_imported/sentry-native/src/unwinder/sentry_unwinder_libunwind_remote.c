// Remote stack unwinding via libunwind ptrace accessors.
// This file must NOT define UNW_LOCAL_ONLY so that unw_init_remote()
// and other generic (_U prefix) symbols are used.
//
// Only compiled into sentry-crash daemon
// (SENTRY_WITH_UNWINDER_LIBUNWIND_REMOTE).
// The sentry library compiles this file too (shared sources) but the
// function body is empty without the define.

#include "sentry_boot.h"
#include "sentry_string.h"
#include "sentry_unwinder.h"

#ifdef SENTRY_WITH_UNWINDER_LIBUNWIND_REMOTE

#    include "sentry_logger.h"

#    include <errno.h>
#    include <string.h>
#    include <sys/ptrace.h>
#    include <sys/wait.h>
#    include <unistd.h>

#    include <libunwind-ptrace.h>
#    include <libunwind.h>

static int
format_register_name(char *dst, size_t dst_len, unw_regnum_t reg)
{
    const char *name = unw_regname(reg);
    if (!name || strcmp(name, "???") == 0) {
        return 0;
    }

    size_t len = strlen(name);
    if (len == 0 || len >= dst_len) {
        return 0;
    }

    memcpy(dst, name, len + 1);
    sentry__string_ascii_lower(dst);
    return 1;
}

static void
capture_registers(unw_cursor_t *cursor, sentry_remote_registers_t *registers)
{
    if (!registers) {
        return;
    }

    for (unw_regnum_t reg = 0; reg <= UNW_REG_LAST
        && registers->count < SENTRY_REMOTE_UNWIND_MAX_REGISTERS;
        reg++) {
        unw_word_t value = 0;
        if (unw_get_reg(cursor, reg, &value) < 0) {
            continue;
        }

        sentry_remote_register_t *remote_register
            = &registers->values[registers->count];
        if (!format_register_name(
                remote_register->name, sizeof(remote_register->name), reg)) {
            continue;
        }

        remote_register->value = (uint64_t)value;
        registers->count++;
    }
}

size_t
sentry__unwind_stack_from_thread_libunwind_remote(pid_t tid,
    sentry_remote_frame_t *frames, size_t max_frames,
    sentry_remote_registers_t *registers)
{
    if (registers) {
        memset(registers, 0, sizeof(*registers));
    }

    if (ptrace(PTRACE_ATTACH, tid, NULL, NULL) != 0) {
        SENTRY_WARNF("remote_unwind: ptrace attach failed for %d: %s", tid,
            strerror(errno));
        return 0;
    }

    // Poll with WNOHANG to avoid hanging on threads in D state. PTRACE_ATTACH
    // sends SIGSTOP but the thread won't stop until it leaves uninterruptible
    // sleep, which may never happen.
    int status = 0;
    int retries = 20;
    for (;;) {
        pid_t waited = waitpid(tid, &status, __WALL | WNOHANG);
        if (waited == tid) {
            break;
        }
        if (waited < 0) {
            SENTRY_WARNF("remote_unwind: waitpid failed for %d: %s", tid,
                strerror(errno));
            ptrace(PTRACE_DETACH, tid, NULL, NULL);
            return 0;
        }
        if (--retries <= 0) {
            SENTRY_WARNF("remote_unwind: waitpid timed out for %d", tid);
            ptrace(PTRACE_DETACH, tid, NULL, NULL);
            return 0;
        }
        usleep(100000);
    }

    if (!WIFSTOPPED(status)) {
        SENTRY_WARNF(
            "remote_unwind: thread %d did not stop after attach: status=%d",
            tid, status);
        ptrace(PTRACE_DETACH, tid, NULL, NULL);
        return 0;
    }

    size_t n = 0;

    unw_addr_space_t as = unw_create_addr_space(&_UPT_accessors, 0);
    if (!as) {
        SENTRY_WARN("remote_unwind: unw_create_addr_space failed");
        goto detach;
    }

    void *upt = _UPT_create(tid);
    if (!upt) {
        SENTRY_WARN("remote_unwind: _UPT_create failed");
        unw_destroy_addr_space(as);
        goto detach;
    }

    unw_cursor_t cursor;
    int ret = unw_init_remote(&cursor, as, upt);
    if (ret < 0) {
        SENTRY_WARNF("remote_unwind: unw_init_remote failed: %d", ret);
        _UPT_destroy(upt);
        unw_destroy_addr_space(as);
        goto detach;
    }

    capture_registers(&cursor, registers);

    while (n < max_frames) {
        unw_word_t ip = 0;
        if (unw_get_reg(&cursor, UNW_REG_IP, &ip) < 0 || ip == 0) {
            break;
        }

        frames[n].ip = (uint64_t)ip;
        frames[n].symbol[0] = '\0';
        frames[n].symbol_offset = 0;

        unw_word_t off = 0;
        int name_ret = unw_get_proc_name(
            &cursor, frames[n].symbol, sizeof(frames[n].symbol), &off);
        // unw_get_proc_name returns -UNW_ENOMEM when the buffer is
        // too small, but still fills it with the truncated name and
        // sets *off to the valid offset.
        if (name_ret == 0 || name_ret == -UNW_ENOMEM) {
            frames[n].symbol_offset = (uint64_t)off;
        }

        n++;

        if (unw_step(&cursor) <= 0) {
            break;
        }
    }

    _UPT_destroy(upt);
    unw_destroy_addr_space(as);

detach:
    ptrace(PTRACE_DETACH, tid, NULL, NULL);

    SENTRY_DEBUGF("Remote unwound %zu frames for thread %d", n, tid);
    return n;
}

#else /* !SENTRY_WITH_UNWINDER_LIBUNWIND_REMOTE */

size_t
sentry__unwind_stack_from_thread_libunwind_remote(pid_t tid,
    sentry_remote_frame_t *frames, size_t max_frames,
    sentry_remote_registers_t *registers)
{
    (void)tid;
    (void)frames;
    (void)max_frames;
    (void)registers;
    return 0;
}

#endif /* SENTRY_WITH_UNWINDER_LIBUNWIND_REMOTE */
