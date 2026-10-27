// Define _DARWIN_C_SOURCE for pthread_threadid_np on macOS
// Must be defined before including any system headers
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#    define _DARWIN_C_SOURCE
#endif

#include "sentry_crash_handler.h"

#include "sentry_alloc.h"
#include "sentry_core.h"
#include "sentry_logger.h"
#include "sentry_os.h"
#include "sentry_sync.h"

#include <string.h>
#include <time.h>

#if defined(SENTRY_PLATFORM_UNIX)
#    include "sentry_unix_pageallocator.h"
#    include <errno.h>
#    include <fcntl.h>
#    include <limits.h>
#    include <pthread.h>
#    include <signal.h>
#    include <sys/stat.h>
#    include <sys/types.h>
#    include <unistd.h>
#elif defined(SENTRY_PLATFORM_WINDOWS)
#    include <tlhelp32.h>
#    include <windows.h>
#    include <winnt.h>
#endif

// signal_safe_memcpy and signal_safe_memzero are only used on Unix
// (for signal handler context copying)
#if defined(SENTRY_PLATFORM_UNIX)
/**
 * Signal-safe memory copy that bypasses TSAN/ASAN interception.
 * Uses volatile to prevent compiler optimization and sanitizer hooks.
 * This is critical for signal/exception handlers where intercepted memcpy is
 * not safe.
 */
static void
signal_safe_memcpy(void *dest, const void *src, size_t n)
{
    volatile unsigned char *d = (volatile unsigned char *)dest;
    const volatile unsigned char *s = (const volatile unsigned char *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

// signal_safe_memzero is only used on macOS (for thread state zeroing)
#    if defined(SENTRY_PLATFORM_MACOS)
/**
 * Signal-safe memory zero that bypasses TSAN/ASAN interception.
 */
static void
signal_safe_memzero(void *dest, size_t n)
{
    volatile unsigned char *d = (volatile unsigned char *)dest;
    for (size_t i = 0; i < n; i++) {
        d[i] = 0;
    }
}
#    endif // SENTRY_PLATFORM_MACOS

#    if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
#        include <dirent.h>
#        include <stdlib.h>
#        include <sys/syscall.h>
#        ifdef SENTRY_WITH_UNWINDER_LIBUNWIND
#            define UNW_LOCAL_ONLY
#            include <libunwind.h>
#        endif
#    endif

#    if defined(SENTRY_PLATFORM_MACOS)
#        include <mach-o/dyld.h>
#        include <mach-o/loader.h>
#        include <mach/mach.h>
#        include <mach/mach_vm.h>
#    endif

// Signals to handle
static const int g_crash_signals[] = {
    SIGABRT,
    SIGBUS,
    SIGFPE,
    SIGILL,
    SIGSEGV,
    SIGSYS,
    SIGTRAP,
};
static const size_t g_crash_signal_count
    = sizeof(g_crash_signals) / sizeof(g_crash_signals[0]);

// Global state (signal-safe)
static sentry_crash_ipc_t *g_crash_ipc = NULL;
static struct sigaction g_previous_handlers[16];
static stack_t g_signal_stack = { 0 };

static void
reset_signal_handlers(void)
{
    for (size_t i = 0; i < g_crash_signal_count; i++) {
        sigaction(g_crash_signals[i], &g_previous_handlers[i], NULL);
    }
}

static void
reraise_signal(int signum)
{
    signal(signum, SIG_DFL);
    raise(signum);
}

static void
invoke_previous_signal_handler(int signum, siginfo_t *info, void *context)
{
    // Re-enable previous signal handlers before re-raising to prevent loops
    reset_signal_handlers();

    for (size_t i = 0; i < g_crash_signal_count; i++) {
        if (g_crash_signals[i] != signum) {
            continue;
        }

        struct sigaction *handler = &g_previous_handlers[i];
        if (handler->sa_handler == SIG_DFL || handler->sa_handler == SIG_IGN) {
            return;
        }

        if (handler->sa_flags & SA_SIGINFO) {
            handler->sa_sigaction(signum, info, context);
        } else {
            handler->sa_handler(signum);
        }

        return;
    }
}

/**
 * Get current thread ID (signal-safe)
 */
static pid_t
get_tid(void)
{
#    if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    return (pid_t)syscall(SYS_gettid);
#    elif defined(SENTRY_PLATFORM_MACOS)
    // Use pthread_threadid_np for a unique thread ID that matches
    // what we store in the crash context. Note: mach_thread_self()
    // would leak a Mach port reference in signal handler context.
    uint64_t tid = 0;
    pthread_threadid_np(pthread_self(), &tid);
    return (pid_t)tid;
#    else
    return getpid();
#    endif
}

/**
 * Safe string length (signal-safe)
 */
static size_t
safe_strlen(const char *s)
{
    size_t len = 0;
    while (s && s[len] != '\0') {
        len++;
    }
    return len;
}

// safe_strncpy is only used on macOS (for stack path and module names)
#    if defined(SENTRY_PLATFORM_MACOS)
/**
 * Safe string copy (signal-safe)
 */
static void
safe_strncpy(char *dest, const char *src, size_t n)
{
    if (!dest || !src || n == 0) {
        return;
    }

    size_t i;
    for (i = 0; i < n - 1 && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

/**
 * Safe unsigned int to string (signal-safe)
 * Returns number of characters written (not including null terminator)
 */
static size_t
safe_uint_to_str(char *buf, size_t buf_size, unsigned int value)
{
    if (!buf || buf_size == 0) {
        return 0;
    }

    // Handle zero case
    if (value == 0) {
        if (buf_size >= 2) {
            buf[0] = '0';
            buf[1] = '\0';
            return 1;
        }
        buf[0] = '\0';
        return 0;
    }

    // Build string in reverse
    char tmp[16];
    size_t len = 0;
    while (value > 0 && len < sizeof(tmp)) {
        tmp[len++] = '0' + (value % 10);
        value /= 10;
    }

    // Check if we have enough space
    if (len >= buf_size) {
        buf[0] = '\0';
        return 0;
    }

    // Reverse into output buffer
    for (size_t i = 0; i < len; i++) {
        buf[i] = tmp[len - 1 - i];
    }
    buf[len] = '\0';
    return len;
}

/**
 * Build stack path signal-safely: "{database_path}/__sentry-stack{index}"
 * Returns total length or 0 on error/truncation
 */
static size_t
safe_build_stack_path(
    char *dest, size_t dest_size, const char *database_path, unsigned int index)
{
    if (!dest || dest_size == 0) {
        return 0;
    }

    // Copy database path
    size_t pos = 0;
    size_t db_len = safe_strlen(database_path);
    if (db_len >= dest_size) {
        dest[0] = '\0';
        return 0; // Would truncate
    }
    safe_strncpy(dest, database_path, dest_size);
    pos = db_len;

    // Append "/__sentry-stack"
    const char *suffix = "/__sentry-stack";
    size_t suffix_len = 15; // strlen("/__sentry-stack")
    if (pos + suffix_len >= dest_size) {
        dest[0] = '\0';
        return 0; // Would truncate
    }
    for (size_t i = 0; i < suffix_len; i++) {
        dest[pos++] = suffix[i];
    }

    // Append index number
    char num_buf[16];
    size_t num_len = safe_uint_to_str(num_buf, sizeof(num_buf), index);
    if (pos + num_len >= dest_size) {
        dest[0] = '\0';
        return 0; // Would truncate
    }
    for (size_t i = 0; i < num_len; i++) {
        dest[pos++] = num_buf[i];
    }

    dest[pos] = '\0';
    return pos;
}
#    endif

/**
 * Signal handler (signal-safe)
 */
static void
crash_signal_handler(int signum, siginfo_t *info, void *context)
{
    sentry_crash_ipc_t *ipc = g_crash_ipc;
    if (!ipc || !ipc->shmem) {
        // No IPC available, forward to the previous handler
        invoke_previous_signal_handler(signum, info, context);
        // The previous handler returned, fall back to default termination
        reraise_signal(signum);
        return;
    }

    // Only handle crash once - check if already processing
    static volatile long handling_crash = 0;
    if (!sentry__atomic_compare_swap(&handling_crash, 0, 1)) {
        // Already handling a crash, just exit immediately
        _exit(1);
    }

    sentry_crash_context_t *ctx = ipc->shmem;
    ucontext_t *uctx = (ucontext_t *)context;

    // Fill crash context
    ctx->crashed_pid = getpid();
    ctx->crashed_tid = get_tid();

#    if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    ctx->platform.signum = signum;
    // Use signal-safe memcpy to avoid TSAN-intercepted memcpy in signal handler
    signal_safe_memcpy(
        &ctx->platform.siginfo, info, sizeof(ctx->platform.siginfo));
    signal_safe_memcpy(
        &ctx->platform.context, uctx, sizeof(ctx->platform.context));

    // Store the crashing thread context
    // Note: We DON'T enumerate threads here using opendir/readdir because
    // they allocate memory (not signal-safe). The daemon's minidump writer
    // will enumerate threads out-of-process by calling enumerate_threads().
    ctx->platform.num_threads = 1;
    ctx->platform.threads[0].tid = ctx->crashed_tid;
    signal_safe_memcpy(&ctx->platform.threads[0].context, uctx,
        sizeof(ctx->platform.threads[0].context));

    // Capture backtrace using libunwind (DWARF-based, works without frame
    // pointers). This runs in the signal handler, which is safe because
    // libunwind's core unwinding API (unw_init_local2, unw_step, unw_get_reg)
    // is async-signal-safe. The daemon will prefer this over FP-based walking.
#        ifdef SENTRY_WITH_UNWINDER_LIBUNWIND
    ctx->platform.backtrace_count = 0;
    {
        unw_cursor_t cursor;
        int ret = unw_init_local2(
            &cursor, (unw_context_t *)uctx, UNW_INIT_SIGNAL_FRAME);
        if (ret == 0) {
            size_t n = 0;
            while (n < SENTRY_CRASH_MAX_BACKTRACE_FRAMES) {
                unw_word_t ip = 0;
                if (unw_get_reg(&cursor, UNW_REG_IP, &ip) < 0 || ip == 0) {
                    break;
                }
                ctx->platform.backtrace_ips[n++] = (uint64_t)ip;
                if (unw_step(&cursor) <= 0) {
                    break;
                }
            }
            ctx->platform.backtrace_count = n;
        }
    }
#        endif

#    elif defined(SENTRY_PLATFORM_MACOS)
    ctx->platform.signum = signum;
    // Use signal-safe memcpy to avoid TSAN-intercepted memcpy in signal handler
    signal_safe_memcpy(
        &ctx->platform.siginfo, info, sizeof(ctx->platform.siginfo));
    // Copy mcontext data (ucontext_t.uc_mcontext is just a pointer)
    signal_safe_memcpy(&ctx->platform.mcontext, uctx->uc_mcontext,
        sizeof(ctx->platform.mcontext));

    // Capture all threads (signal-safe on macOS)
    ctx->platform.num_threads = 0;
    task_t task = mach_task_self();
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t thread_count = 0;

    // Get the crashing thread port without incrementing the reference count.
    // mach_thread_self() would leak the port since mach_port_deallocate is
    // not async-signal-safe.
    thread_t crashing_thread = pthread_mach_thread_np(pthread_self());

    kern_return_t kr = task_threads(task, &threads, &thread_count);
    if (kr == KERN_SUCCESS) {
        // Limit to available space
        if (thread_count > SENTRY_CRASH_MAX_THREADS) {
            thread_count = SENTRY_CRASH_MAX_THREADS;
        }

        for (mach_msg_type_number_t i = 0; i < thread_count; i++) {
            ctx->platform.threads[i].thread = threads[i];

            // Get thread ID (portable across processes)
            thread_identifier_info_data_t identifier_info;
            mach_msg_type_number_t identifier_info_count
                = THREAD_IDENTIFIER_INFO_COUNT;
            if (thread_info(threads[i], THREAD_IDENTIFIER_INFO,
                    (thread_info_t)&identifier_info, &identifier_info_count)
                == KERN_SUCCESS) {
                ctx->platform.threads[i].tid = identifier_info.thread_id;
            } else {
                ctx->platform.threads[i].tid = 0;
            }

            // Get thread name from THREAD_EXTENDED_INFO
            ctx->platform.threads[i].name[0] = '\0';
            {
                thread_extended_info_data_t extended_info;
                mach_msg_type_number_t ext_count = THREAD_EXTENDED_INFO_COUNT;
                if (thread_info(threads[i], THREAD_EXTENDED_INFO,
                        (thread_info_t)&extended_info, &ext_count)
                    == KERN_SUCCESS) {
                    extended_info.pth_name[sizeof(extended_info.pth_name) - 1]
                        = '\0';
                    signal_safe_memcpy(ctx->platform.threads[i].name,
                        extended_info.pth_name,
                        sizeof(ctx->platform.threads[i].name));
                    ctx->platform.threads[i]
                        .name[sizeof(ctx->platform.threads[i].name) - 1]
                        = '\0';
                }
            }

            // For the crashing thread, use the context from the signal handler
            // For other threads, use thread_get_state()
            bool is_crashing_thread = (threads[i] == crashing_thread);

            if (is_crashing_thread) {
                // Use register state from signal handler context
#        if defined(__x86_64__)
                ctx->platform.threads[i].state.__ss.__rax
                    = uctx->uc_mcontext->__ss.__rax;
                ctx->platform.threads[i].state.__ss.__rbx
                    = uctx->uc_mcontext->__ss.__rbx;
                ctx->platform.threads[i].state.__ss.__rcx
                    = uctx->uc_mcontext->__ss.__rcx;
                ctx->platform.threads[i].state.__ss.__rdx
                    = uctx->uc_mcontext->__ss.__rdx;
                ctx->platform.threads[i].state.__ss.__rdi
                    = uctx->uc_mcontext->__ss.__rdi;
                ctx->platform.threads[i].state.__ss.__rsi
                    = uctx->uc_mcontext->__ss.__rsi;
                ctx->platform.threads[i].state.__ss.__rbp
                    = uctx->uc_mcontext->__ss.__rbp;
                ctx->platform.threads[i].state.__ss.__rsp
                    = uctx->uc_mcontext->__ss.__rsp;
                ctx->platform.threads[i].state.__ss.__r8
                    = uctx->uc_mcontext->__ss.__r8;
                ctx->platform.threads[i].state.__ss.__r9
                    = uctx->uc_mcontext->__ss.__r9;
                ctx->platform.threads[i].state.__ss.__r10
                    = uctx->uc_mcontext->__ss.__r10;
                ctx->platform.threads[i].state.__ss.__r11
                    = uctx->uc_mcontext->__ss.__r11;
                ctx->platform.threads[i].state.__ss.__r12
                    = uctx->uc_mcontext->__ss.__r12;
                ctx->platform.threads[i].state.__ss.__r13
                    = uctx->uc_mcontext->__ss.__r13;
                ctx->platform.threads[i].state.__ss.__r14
                    = uctx->uc_mcontext->__ss.__r14;
                ctx->platform.threads[i].state.__ss.__r15
                    = uctx->uc_mcontext->__ss.__r15;
                ctx->platform.threads[i].state.__ss.__rip
                    = uctx->uc_mcontext->__ss.__rip;
                ctx->platform.threads[i].state.__ss.__rflags
                    = uctx->uc_mcontext->__ss.__rflags;
                ctx->platform.threads[i].state.__ss.__cs
                    = uctx->uc_mcontext->__ss.__cs;
                ctx->platform.threads[i].state.__ss.__fs
                    = uctx->uc_mcontext->__ss.__fs;
                ctx->platform.threads[i].state.__ss.__gs
                    = uctx->uc_mcontext->__ss.__gs;
#        elif defined(__aarch64__)
                // Copy entire thread state struct. This preserves raw
                // register values including PAC-signed pointers on arm64e.
                ctx->platform.threads[i].state.__ss = uctx->uc_mcontext->__ss;
#        endif
            } else {
                // Capture thread state from thread_get_state for other threads
                // Note: thread_get_state writes to thread_state_t, which is
                // __ss (not the full mcontext), so we must pass &state.__ss.
                // Use the explicit 64-bit flavor; MACHINE_THREAD_STATE is the
                // unified flavor whose 8-byte header would shift every
                // register.
#        if defined(__aarch64__)
                const thread_state_flavor_t state_flavor = ARM_THREAD_STATE64;
                mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
#        elif defined(__x86_64__)
                const thread_state_flavor_t state_flavor = x86_THREAD_STATE64;
                mach_msg_type_number_t state_count = x86_THREAD_STATE64_COUNT;
#        else
                const thread_state_flavor_t state_flavor = MACHINE_THREAD_STATE;
                mach_msg_type_number_t state_count = MACHINE_THREAD_STATE_COUNT;
#        endif
                kern_return_t state_kr
                    = thread_get_state(threads[i], state_flavor,
                        (thread_state_t)&ctx->platform.threads[i].state.__ss,
                        &state_count);
                if (state_kr != KERN_SUCCESS) {
                    // Failed to get state, but continue with other threads
                    signal_safe_memzero(&ctx->platform.threads[i].state,
                        sizeof(ctx->platform.threads[i].state));
                    ctx->platform.threads[i].stack_path[0] = '\0';
                    ctx->platform.threads[i].stack_size = 0;
                    continue;
                }
            }

            // Capture stack memory for this thread
            uint64_t sp;
#        if defined(__x86_64__)
            sp = ctx->platform.threads[i].state.__ss.__rsp;
#        elif defined(__aarch64__)
            sp = SENTRY__ARM64_GET_SP(ctx->platform.threads[i].state.__ss);
#        else
            sp = 0;
#        endif

            if (sp > 0) {
                // Query stack bounds using vm_region (signal-safe)
                mach_vm_address_t address = sp;
                mach_vm_size_t region_size = 0;
                vm_region_basic_info_data_64_t info;
                mach_msg_type_number_t info_count
                    = VM_REGION_BASIC_INFO_COUNT_64;
                mach_port_t object_name;

                kern_return_t kr = mach_vm_region(task, &address, &region_size,
                    VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                    &info_count, &object_name);

                size_t actual_stack_size = 0;
                if (kr == KERN_SUCCESS) {
                    // Stack region found - capture from SP to end of region
                    uint64_t region_end = address + region_size;
                    if (sp >= address && sp < region_end) {
                        actual_stack_size = region_end - sp;
                    }
                }

                // Fallback: if vm_region failed or returned unreasonable size,
                // use a safe maximum (512KB is typical stack size)
                if (actual_stack_size == 0
                    || actual_stack_size > SENTRY_CRASH_MAX_STACK_CAPTURE) {
                    actual_stack_size = SENTRY_CRASH_MAX_STACK_CAPTURE;
                }

                if (actual_stack_size > 0) {
                    // Create stack file path in database directory
                    // (signal-safe)
                    char stack_path[SENTRY_CRASH_MAX_PATH];
                    size_t len = safe_build_stack_path(
                        stack_path, sizeof(stack_path), ctx->database_path, i);

                    // Check for failure/truncation
                    if (len == 0) {
                        continue; // Skip this thread if path too long
                    }

                    // Open and write stack memory (signal-safe)
                    int stack_fd
                        = open(stack_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                    if (stack_fd >= 0) {
                        // Write stack memory from SP upwards
                        ssize_t written
                            = write(stack_fd, (void *)sp, actual_stack_size);
                        close(stack_fd);

                        if (written > 0) {
                            // Successfully saved stack (even if partial)
                            safe_strncpy(ctx->platform.threads[i].stack_path,
                                stack_path,
                                sizeof(ctx->platform.threads[i].stack_path));
                            ctx->platform.threads[i].stack_size
                                = (size_t)written;
                        } else {
                            ctx->platform.threads[i].stack_path[0] = '\0';
                            ctx->platform.threads[i].stack_size = 0;
                        }
                    } else {
                        ctx->platform.threads[i].stack_path[0] = '\0';
                        ctx->platform.threads[i].stack_size = 0;
                    }
                } else {
                    ctx->platform.threads[i].stack_path[0] = '\0';
                    ctx->platform.threads[i].stack_size = 0;
                }
            } else {
                ctx->platform.threads[i].stack_path[0] = '\0';
                ctx->platform.threads[i].stack_size = 0;
            }
        }
        ctx->platform.num_threads = thread_count;

        // Don't deallocate threads array here - will be done by daemon
        // The thread ports remain valid across processes
    } else {
        // task_threads failed - this might happen from signal handler
        // Fall back to just capturing the crashing thread
        ctx->platform.num_threads = 0;
    }

    // Capture module information from dyld (signal-safe on macOS)
    ctx->module_count = 0;
    uint32_t image_count = _dyld_image_count();
    if (image_count > SENTRY_CRASH_MAX_MODULES) {
        image_count = SENTRY_CRASH_MAX_MODULES;
    }

    for (uint32_t i = 0;
        i < image_count && ctx->module_count < SENTRY_CRASH_MAX_MODULES; i++) {
        const struct mach_header *header = _dyld_get_image_header(i);
        const char *name = _dyld_get_image_name(i);
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);

        if (!header || !name) {
            continue;
        }

        sentry_module_info_t *module = &ctx->modules[ctx->module_count++];
        // _dyld_get_image_header() returns the actual loaded address (slide
        // already applied) We use the header address directly as the base
        // address for symbolication
        (void)
            slide; // Slide is informational only - not needed for base_address
        module->base_address = (uint64_t)header;

        // Calculate module size and extract UUID (signal-safe)
        uint64_t size = 0;
        signal_safe_memzero(module->uuid, sizeof(module->uuid));
        module->pdb_age = 0; // Not used on macOS, only for Windows PE modules

        if (header->magic == MH_MAGIC_64 || header->magic == MH_CIGAM_64) {
            const struct mach_header_64 *header64
                = (const struct mach_header_64 *)header;
            const uint8_t *cmds = (const uint8_t *)(header64 + 1);
            bool has_size = false;
            bool has_uuid = false;

            for (uint32_t j = 0;
                j < header64->ncmds && j < 256 && (!has_size || !has_uuid);
                j++) {
                const struct load_command *cmd
                    = (const struct load_command *)cmds;

                if (cmd->cmd == LC_SEGMENT_64) {
                    const struct segment_command_64 *seg
                        = (const struct segment_command_64 *)cmd;
                    // image_size on macOS is the __TEXT segment's vmsize:
                    // https://github.com/getsentry/sentry-native/blob/82dec4b8518de32586c4a761a3a9df4aed9d7b8d/src/modulefinder/sentry_modulefinder_apple.c#L65-L69
                    if (memcmp(seg->segname, "__TEXT", 7) == 0) {
                        size = seg->vmsize;
                        has_size = true;
                    }
                } else if (cmd->cmd == LC_UUID) {
                    // Extract UUID for symbolication
                    const struct uuid_command *uuid_cmd
                        = (const struct uuid_command *)cmd;
                    signal_safe_memcpy(module->uuid, uuid_cmd->uuid, 16);
                    has_uuid = true;
                }

                cmds += cmd->cmdsize;
                if (cmd->cmdsize == 0)
                    break; // Prevent infinite loop
            }
        }
        module->size = size;

        // Copy module name (signal-safe)
        safe_strncpy(module->name, name, sizeof(module->name));
    }

    // Save module header pages to a single file for the daemon.
    // The first 4096 bytes of each module contain the Mach-O header and load
    // commands. System dylibs in the shared cache can't be read from disk,
    // but we can read them from this process's address space.
    // File format: module[0] header (4096 bytes) || module[1] header || ...
    {
        char hdr_path[SENTRY_CRASH_MAX_PATH];
        size_t pos = safe_strlen(ctx->database_path);
        if (pos + 22 < sizeof(hdr_path)) { // "/__sentry-modheaders\0"
            safe_strncpy(hdr_path, ctx->database_path, sizeof(hdr_path));
            const char *suffix = "/__sentry-modheaders";
            for (size_t si = 0; suffix[si] != '\0'; si++) {
                hdr_path[pos++] = suffix[si];
            }
            hdr_path[pos] = '\0';

            int hdr_fd = open(hdr_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (hdr_fd >= 0) {
                // Zero page used as fallback when write fails, ensuring
                // each module's slot stays aligned at 4096-byte boundaries.
                static const char zero_page[4096] = { 0 };
                for (uint32_t mi = 0; mi < ctx->module_count; mi++) {
                    const void *base = (const void *)(uintptr_t)ctx->modules[mi]
                                           .base_address;
                    ssize_t written = write(hdr_fd, base, 4096);
                    if (written < 0) {
                        // Write failed (EFAULT etc.) - write zeros to
                        // maintain alignment for subsequent modules.
                        write(hdr_fd, zero_page, 4096);
                    } else if (written < 4096) {
                        // Short write - pad with zeros to maintain
                        // alignment.
                        write(hdr_fd, zero_page, (size_t)(4096 - written));
                    }
                }
                close(hdr_fd);
            }
        }
    }
#    endif

    // Enable signal-safe page allocator before calling exception handler
    // This allows malloc/free to work safely in signal handler context
#    ifdef SENTRY_PLATFORM_UNIX
    sentry__page_allocator_enable();
#    endif

    // Call Sentry's exception handler to invoke on_crash/before_send hooks
    // Note: With page allocator enabled, this is now signal-safe
    sentry_ucontext_t sentry_uctx;
    sentry_uctx.signum = signum;
    sentry_uctx.siginfo = info;
    sentry_uctx.user_context = uctx;
    sentry_handle_exception(&sentry_uctx);

    // Try to notify daemon
    if (sentry__atomic_compare_swap(&ctx->state, SENTRY_CRASH_STATE_READY,
            SENTRY_CRASH_STATE_CRASHED)) {

        // Successfully claimed crash slot, notify daemon
        sentry__crash_ipc_notify(ipc);

        // Wait for daemon to finish processing (keep process alive for
        // minidump)
        bool processing_started = false;
        int elapsed_ms = 0;
        while (elapsed_ms < SENTRY_CRASH_HANDLER_WAIT_TIMEOUT_MS) {
            long state = sentry__atomic_fetch(&ctx->state);
            if (state == SENTRY_CRASH_STATE_PROCESSING && !processing_started) {
                // Daemon started processing (no logging - signal-safe)
                processing_started = true;
            } else if (state >= SENTRY_CRASH_STATE_CAPTURED) {
                // Daemon captured crash data (no logging - signal-safe)
                goto daemon_handling;
            }

            // Sleep using poll interval (signal-safe)
            struct timespec ts = { .tv_sec = 0,
                .tv_nsec = SENTRY_CRASH_HANDLER_POLL_INTERVAL_MS * 1000000LL };
            nanosleep(&ts, NULL);
            elapsed_ms += SENTRY_CRASH_HANDLER_POLL_INTERVAL_MS;
        }

        // Timeout (no logging - signal-safe)
    }

daemon_handling:
    // Re-raise signal to let system handle it
    SENTRY_DEBUG("Wait complete, allowing process to terminate");

    // Dump daemon log for debugging (uses stdio, safe after page allocator
    // enabled)
    // Extract the shm identifier for log path construction
    // macOS: shm_path = "{tmpdir}/.sentry-shm-{id}", Linux: shm_name =
    // "/s-{id}"
#    if defined(SENTRY_PLATFORM_MACOS)
    const char *shm_id_src = ipc ? ipc->shm_path : "";
#    else
    const char *shm_id_src = ipc ? ipc->shm_name : "";
#    endif
    if (shm_id_src[0] != '\0' && ctx && ctx->database_path[0] != '\0') {
        // Extract hex ID after last '-' in shm name/path
        const char *shm_id = NULL;
        for (const char *p = shm_id_src; *p; p++) {
            if (*p == '-') {
                shm_id = p + 1;
            }
        }

        if (shm_id) {
            char log_path[SENTRY_CRASH_MAX_PATH];
            int len = 0;
            // Manually build path string (signal-safe)
            for (const char *p = ctx->database_path;
                *p && len < (int)sizeof(log_path) - 30; p++) {
                log_path[len++] = *p;
            }
            const char *suffix = "/sentry-daemon-";
            for (const char *p = suffix; *p && len < (int)sizeof(log_path) - 15;
                p++) {
                log_path[len++] = *p;
            }
            for (const char *p = shm_id; *p && len < (int)sizeof(log_path) - 5;
                p++) {
                log_path[len++] = *p;
            }
            const char *ext = ".log";
            for (const char *p = ext; *p && len < (int)sizeof(log_path) - 1;
                p++) {
                log_path[len++] = *p;
            }
            log_path[len] = '\0';

            // Try to open and dump log file
            int fd = open(log_path, O_RDONLY);
            if (fd >= 0) {
                // Use sizeof()-1 for string literals (signal-safe)
                ssize_t rv = write(STDERR_FILENO, "\n========== Daemon Log (",
                    sizeof("\n========== Daemon Log (") - 1);
                (void)rv; // Ignore write errors in signal handler
                rv = write(STDERR_FILENO, shm_id, safe_strlen(shm_id));
                (void)rv;
                rv = write(STDERR_FILENO, ") ==========\n",
                    sizeof(") ==========\n") - 1);
                (void)rv;

                char buf[1024];
                ssize_t n;
                while ((n = read(fd, buf, sizeof(buf))) > 0) {
                    rv = write(STDERR_FILENO, buf, n);
                    (void)rv;
                }

                rv = write(STDERR_FILENO,
                    "=========================================\n\n",
                    sizeof("=========================================\n\n")
                        - 1);
                (void)rv;
                close(fd);
            }
        }
    }

    invoke_previous_signal_handler(signum, info, context);

#    if defined(SENTRY_PLATFORM_MACOS)
    if (!ctx->system_crash_reporter_enabled) {
        // By convention, use the 128 + signal exit code without re-raising
        // and invoking the macOS system crash reporter
        _exit(128 + signum);
    }
#    endif

    // The previous handler returned, fall back to default termination
    reraise_signal(signum);
}

int
sentry__crash_handler_init(sentry_crash_ipc_t *ipc)
{
    if (!ipc) {
        return -1;
    }

    g_crash_ipc = ipc;

    // Set up signal stack
    g_signal_stack.ss_sp = sentry_malloc(SENTRY_CRASH_SIGNAL_STACK_SIZE);
    if (!g_signal_stack.ss_sp) {
        SENTRY_WARN("failed to allocate signal stack");
        return -1;
    }

    g_signal_stack.ss_size = SENTRY_CRASH_SIGNAL_STACK_SIZE;
    g_signal_stack.ss_flags = 0;

    if (sigaltstack(&g_signal_stack, NULL) < 0) {
        SENTRY_WARNF("failed to set signal stack: %s", strerror(errno));
        sentry_free(g_signal_stack.ss_sp);
        g_signal_stack.ss_sp = NULL;
        return -1;
    }

    // Install signal handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = crash_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;

    size_t installed_count = 0;
    for (size_t i = 0; i < g_crash_signal_count; i++) {
        int sig = g_crash_signals[i];
        if (sigaction(sig, &sa, &g_previous_handlers[i]) < 0) {
            SENTRY_WARNF("failed to install handler for signal %d: %s", sig,
                strerror(errno));
            // Rollback all previously installed handlers
            for (size_t j = 0; j < installed_count; j++) {
                sigaction(g_crash_signals[j], &g_previous_handlers[j], NULL);
            }
            // Clean up signal stack
            g_signal_stack.ss_flags = SS_DISABLE;
            sigaltstack(&g_signal_stack, NULL);
            sentry_free(g_signal_stack.ss_sp);
            g_signal_stack.ss_sp = NULL;
            return -1;
        }
        installed_count++;
    }

    // Prime libunwind's internal cache by performing a short unwind.
    // This forces dl_iterate_phdr to be called now (while safe) rather than
    // in the signal handler where it could deadlock if the crash occurs
    // during dlopen/dlclose.
#    if (defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID))   \
        && defined(SENTRY_WITH_UNWINDER_LIBUNWIND)
    {
        unw_context_t uc;
        unw_cursor_t cursor;
#        ifdef __clang__
#            pragma clang diagnostic push
#            pragma clang diagnostic ignored                                   \
                "-Wgnu-statement-expression-from-macro-expansion"
#        endif
        if (unw_getcontext(&uc) == 0) {
#        ifdef __clang__
#            pragma clang diagnostic pop
#        endif
            if (unw_init_local(&cursor, &uc) == 0) {
                // Walk a few frames to ensure .eh_frame info is cached
                for (int i = 0; i < 5 && unw_step(&cursor) > 0; i++) {
                    // intentionally empty - just priming the cache
                }
            }
        }
        SENTRY_DEBUG("libunwind cache primed");
    }
#    endif

    SENTRY_DEBUG("crash handler initialized");
    return 0;
}

void
sentry__crash_handler_shutdown(void)
{
    // Restore previous signal handlers
    reset_signal_handlers();

    // Clean up signal stack
    if (g_signal_stack.ss_sp) {
        g_signal_stack.ss_flags = SS_DISABLE;
        sigaltstack(&g_signal_stack, NULL);
        sentry_free(g_signal_stack.ss_sp);
        g_signal_stack.ss_sp = NULL;
    }

    g_crash_ipc = NULL;

    SENTRY_DEBUG("crash handler shutdown");
}

#elif defined(SENTRY_PLATFORM_WINDOWS)

// Global state for Windows exception handling
static sentry_crash_ipc_t *g_crash_ipc = NULL;
static LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = NULL;

static LONG WINAPI crash_exception_filter(EXCEPTION_POINTERS *exception_info);

static void
crash_sigabrt_handler(EXCEPTION_POINTERS *exception_pointers)
{
    crash_exception_filter(exception_pointers);
}

/**
 * Windows exception filter (crash handler)
 */
static LONG WINAPI
crash_exception_filter(EXCEPTION_POINTERS *exception_info)
{
    // Only handle crash once
    static volatile long handling_crash = 0;
    if (!sentry__atomic_compare_swap(&handling_crash, 0, 1)) {
        // Already handling a crash (no logging - exception filter context)
        return EXCEPTION_CONTINUE_SEARCH;
    }

    sentry_crash_ipc_t *ipc = g_crash_ipc;
    if (!ipc || !ipc->shmem) {
        // No IPC or shared memory (no logging - exception filter context)
        return EXCEPTION_CONTINUE_SEARCH;
    }

    sentry_crash_context_t *ctx = ipc->shmem;

    // Fill crash context
    ctx->crashed_pid = GetCurrentProcessId();
    ctx->crashed_tid = GetCurrentThreadId();

    // Store exception information
    ctx->platform.exception_code
        = exception_info->ExceptionRecord->ExceptionCode;
    ctx->platform.exception_record = *exception_info->ExceptionRecord;
    ctx->platform.context = *exception_info->ContextRecord;
    // Store original exception pointers for out-of-process minidump writing
    ctx->platform.exception_pointers = exception_info;

    // Store only the crashing thread's context
    // Note: We intentionally do NOT suspend other threads to capture their
    // contexts here. Suspending threads in an exception filter is dangerous:
    // - If a suspended thread holds the heap lock, we may deadlock on
    // allocation
    // - If a suspended thread holds the loader lock, any DLL call may deadlock
    // Instead, we rely on MiniDumpWriteDump (called by the daemon process)
    // which safely captures all thread contexts from outside the crashed
    // process using the debugger API with ClientPointers=TRUE.
    ctx->platform.num_threads = 1;
    ctx->platform.threads[0].thread_id = GetCurrentThreadId();
    ctx->platform.threads[0].context = *exception_info->ContextRecord;

    // Call Sentry's exception handler
    sentry_ucontext_t sentry_uctx = { 0 };
    sentry_uctx.exception_ptrs = *exception_info;
    sentry_handle_exception(&sentry_uctx);

    bool swap_result = sentry__atomic_compare_swap(
        &ctx->state, SENTRY_CRASH_STATE_READY, SENTRY_CRASH_STATE_CRASHED);

    if (swap_result) {
        // Successfully claimed crash slot, notify daemon
        sentry__crash_ipc_notify(ipc);

        // Wait for daemon to finish processing (keep process alive for
        // minidump)
        bool processing_started = false;
        int elapsed_ms = 0;
        while (elapsed_ms < SENTRY_CRASH_HANDLER_WAIT_TIMEOUT_MS) {
            long state = sentry__atomic_fetch(&ctx->state);
            if (state == SENTRY_CRASH_STATE_PROCESSING && !processing_started) {
                // Daemon started processing (no logging - exception filter
                // context)
                processing_started = true;
            } else if (state >= SENTRY_CRASH_STATE_CAPTURED) {
                // Daemon captured crash data (no logging - exception filter
                // context)
                break;
            }
            Sleep(SENTRY_CRASH_HANDLER_POLL_INTERVAL_MS);
            elapsed_ms += SENTRY_CRASH_HANDLER_POLL_INTERVAL_MS;
        }

        // Timeout or completion (no logging - exception filter context)
    }

    // Continue to default handler (which will terminate the process)
    return EXCEPTION_CONTINUE_SEARCH;
}

int
sentry__crash_handler_init(sentry_crash_ipc_t *ipc)
{
    if (!ipc) {
        return -1;
    }

    g_crash_ipc = ipc;

    // Install exception filter
    g_previous_filter = SetUnhandledExceptionFilter(crash_exception_filter);
    sentry__win32_install_sigabrt_handler(crash_sigabrt_handler);

    SENTRY_DEBUG("crash handler initialized (Windows SEH)");
    return 0;
}

void
sentry__crash_handler_shutdown(void)
{
    // Restore previous exception filter
    if (g_previous_filter) {
        SetUnhandledExceptionFilter(g_previous_filter);
        g_previous_filter = NULL;
    }

    sentry__win32_restore_sigabrt_handler();

    g_crash_ipc = NULL;

    SENTRY_DEBUG("crash handler shutdown");
}

#endif // SENTRY_PLATFORM_WINDOWS
