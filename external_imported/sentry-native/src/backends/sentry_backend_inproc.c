#include "sentry_attachment.h"
#include "sentry_boot.h"

#include "sentry_alloc.h"
#include "sentry_backend.h"
#include "sentry_core.h"
#include "sentry_cpu_relax.h"
#include "sentry_database.h"
#include "sentry_envelope.h"
#include "sentry_logger.h"
#include "sentry_logs.h"
#include "sentry_metrics.h"
#include "sentry_options.h"
#if defined(SENTRY_PLATFORM_WINDOWS)
#    include "sentry_os.h"
#endif
#include "sentry_scope.h"
#include "sentry_screenshot.h"
#include "sentry_session_replay.h"
#include "sentry_sync.h"
#include "sentry_tracing.h"
#include "sentry_transport.h"
#include "sentry_unix_pageallocator.h"
#include "transports/sentry_disk_transport.h"
#include <errno.h>
#include <limits.h>
#ifdef SENTRY_PLATFORM_UNIX
#    include <poll.h>
#endif
#ifdef SENTRY_PLATFORM_ANDROID
#    include <android/api-level.h>
#    include <sys/syscall.h>
#endif
#include <string.h>

/**
 * Inproc Backend Introduction
 *
 * As the name suggests the inproc backend runs the crash handling entirely
 * inside the process and thus is the right choice for platforms that
 * are limited in process creation/spawning/cloning (or even deploying a
 * separate release artifact like with `crashpad`). It is also very lightweight
 * in terms of toolchain dependencies because it does not require a C++ standard
 * library.
 *
 * It targets UNIX and Windows (effectively supporting all target platforms of
 * the Native SDK) and uses POSIX signal handling on UNIX and unhandled
 * exception filters (UEF) on Windows. Whenever a signal handler is mentioned
 * in the code or comments, one can replace that with UEF on Windows.
 *
 * In its current implementation it only gathers the crash context for the
 * crashed thread and does not attempt to stop any other threads. While this
 * can be considered a downside for some users, it allows additional handlers
 * to process the crashed process again, which the other backends currently
 * can't guarantee to work. Additional crash signals coming from other threads
 * will be blocked indefinitely until previous handler takes over.
 *
 * The inproc backend splits the handler into two parts:
 *   - a signal handler/unhandled exception filter that severely limits what we
 *     can do, focusing on response to the OS mechanism and almost zero policy.
 *   - a separate handler thread that does most of the typical sentry error
 *     handling and policy implementation, with a bit more freedom.
 *
 * Only if the handler thread has crashed or is otherwise unavailable, will we
 * execute the unsafe part inside the signal-handler itself, as a last chance
 * fallback for report creation. The signal-handler part should not use any
 * synchronization or signal-unsafe function from `libc` (see function-level
 * comment), even access to options is ideally done before during
 * initialization. If access to option or scope (or any other global context)
 * is required this should happen in the handler thread.
 *
 * The handler thread is started during backend initialization and will be
 * triggered by the signal handler via a POSIX pipe on which the handler thread
 * blocks from the start (similarly on Windows, which uses a Semaphore). While
 * the handler thread handles a crash, the signal handler (or UEF) blocks itself
 * on an ACK pipe/semaphore. Once the handler thread is done processing the
 * crash, it will unblock the signal handler which resets the synchronization
 * during crash handling and invokes the handler chain.
 *
 * The most important functions and their meaning:
 *
 *  - `handle_signal`/`handle_exception`: top-level entry points called directly
 *    from the operating system. They pack sentry_ucontext_t and call...
 *  - `process_ucontext`: the actual signal-handler/UEF, primarily manages the
 *    interaction with the OS and other handlers and calls...
 *  - `dispatch_ucontext`: this is the place that decides on where to run the
 *    sentry error event creation and that does the synchronization with...
 *  - `handler_thread_main`: implements the handler thread loop, blocks until
 *    unblocked by the signal handler and finally calls...
 *  - `process_ucontext_deferred`: the implementation of sentry specific
 *    handler policy leading to crash event construction, it defers to...
 *  - `make_signal_event`: that is purely about making a crash event object and
 *    filling the context data
 *
 * The `on_crash` and `before_send` hook usually run on the handler thread
 * during `process_ucontext_deferred` but users cannot rely on any particular
 * thread to call their callbacks. However, they can be sure that the crashed
 * thread won't progress during the execution of their callback code.
 *
 * Note on unwinders:
 *
 * The backend relies on an unwinder that can backtrace from a user context.
 * This is important because the unwinder usually runs in the context of the
 * handler thread, where a direct backtrace makes no longer any sense (even if
 * it was signal safe). We do not dispatch to the handler thread for targets
 * that still use `libbacktrace`, and instead run the unsafe part directly in
 * the signal handler. This is primarily to not break these target, but in
 * general the `libbacktrace`-based unwinder should be considered deprecated.
 *
 * Notes on signal handling in other runtimes:
 *
 * The .net runtimes currently rely on signal handling to deliver managed
 * exceptions caused from the generated native code. Due to the initialization
 * order the inproc backend will receive those signals which it should not
 * process. On setups like these it offers a handler strategy that chains the
 * previous signal handler first, allowing the .net runtime handler to either
 * immediately jump back into runtime code or reset IP/SP so that the returning
 * signal handler continues from the managed exception rather than the crashed
 * instruction.
 *
 * The Android runtime (ART) otoh, while also relying heavily on signal handling
 * to communicate between generated code and the garbage collector entirely
 * shields the signals from us (via `libsigchain` special handler ordering) and
 * only forwards signals that are not relevant to runtime. However, it relies on
 * each thread having a specific sigaltstack setup, which can lead to crashes if
 * overridden. For this reason, we do not set the sigaltstack of any thread if
 * one was already configured even if the size is smaller than we'd want. Since
 * most of the handler runs in a separate thread the size limitation of any pre-
 * configured `sigaltstack` is not a problem to our more complex handler code.
 */

#define SIGNAL_DEF(Sig, Desc) { Sig, #Sig, Desc }
#define MAX_FRAMES 128

// the data exchange between the signal handler and the handler thread
typedef struct sentry_inproc_handler_state_s {
    sentry_ucontext_t uctx;
#ifdef SENTRY_PLATFORM_UNIX
    siginfo_t siginfo_storage;
    ucontext_t user_context_storage;
#endif
    const struct signal_slot *sig_slot;
} sentry_inproc_handler_state_t;

// "data" struct containing options to prevent mutex access in signal handler
typedef struct sentry_inproc_backend_config_s {
    bool enable_logging_when_crashed;
    sentry_handler_strategy_t handler_strategy;
} sentry_inproc_backend_config_t;

// global instance for data-exchange between signal handler and handler thread
static sentry_inproc_handler_state_t g_handler_state;
// global instance for backend configuration state
static sentry_inproc_backend_config_t g_backend_config;

// handler thread state and synchronization variables
static sentry_threadid_t g_handler_thread;
// true once the handler thread starts waiting
static volatile long g_handler_thread_ready = 0;
// shutdown loop invariant
static volatile long g_handler_should_exit = 0;
// signal handler tells handler thread to start working
static volatile long g_handler_has_work = 0;
// State machine for crash handling coordination across threads:
//   IDLE (0):     No crash being handled, ready to accept
//   HANDLING (1): A crash is being processed by another thread
//   DONE (2):     Crash handling complete, signal handlers reset
//
// Threads that crash while state is HANDLING will spin until state becomes
// DONE, then return from their signal handler. Since our signal handlers are
// unregistered before transitioning to DONE, re-executing the crashing
// instruction will invoke the default/previous handler (terminating the
// process) rather than re-entering our handler.
#define CRASH_STATE_IDLE 0
#define CRASH_STATE_HANDLING 1
#define CRASH_STATE_DONE 2
static volatile long g_crash_handling_state = CRASH_STATE_IDLE;

// Set once handlers were installed via sentry__backend_preload(). In this
// mode sentry may keep its chain position across sentry_close() until either
// full init reuses it, or a pre-init/post-close signal falls through, and
// consumes the preload state.
#ifdef SENTRY_PLATFORM_UNIX
static volatile long g_preloaded = 0;
#endif

// trigger/schedule primitives that block the other side until this side is done
#ifdef SENTRY_PLATFORM_UNIX
static int g_handler_pipe[2] = { -1, -1 };
static int g_handler_ack_pipe[2] = { -1, -1 };
static int g_handler_ready_pipe[2] = { -1, -1 };
#elif defined(SENTRY_PLATFORM_WINDOWS)
static HANDLE g_handler_semaphore = NULL;
static HANDLE g_handler_ack_semaphore = NULL;
static HANDLE g_handler_ready_event = NULL;
#endif

#ifdef SENTRY_PLATFORM_UNIX
static void
close_pipe(int pipe_fds[2])
{
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
        pipe_fds[0] = -1;
    }
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
        pipe_fds[1] = -1;
    }
}
#endif

static int
init_handler_sync(void)
{
#ifdef SENTRY_PLATFORM_UNIX
    if (pipe(g_handler_pipe) != 0) {
        SENTRY_WARNF("failed to create handler pipe: %s", strerror(errno));
        return 1;
    }
    if (pipe(g_handler_ack_pipe) != 0) {
        SENTRY_WARNF("failed to create handler ack pipe: %s", strerror(errno));
        close_pipe(g_handler_pipe);
        return 1;
    }
    if (pipe(g_handler_ready_pipe) != 0) {
        SENTRY_WARNF(
            "failed to create handler ready pipe: %s", strerror(errno));
        close_pipe(g_handler_pipe);
        close_pipe(g_handler_ack_pipe);
        return 1;
    }
#elif defined(SENTRY_PLATFORM_WINDOWS)
    g_handler_semaphore = CreateSemaphoreW(NULL, 0, LONG_MAX, NULL);
    if (!g_handler_semaphore) {
        SENTRY_WARN("failed to create handler semaphore");
        return 1;
    }
    g_handler_ack_semaphore = CreateSemaphoreW(NULL, 0, LONG_MAX, NULL);
    if (!g_handler_ack_semaphore) {
        SENTRY_WARN("failed to create handler ack semaphore");
        CloseHandle(g_handler_semaphore);
        g_handler_semaphore = NULL;
        return 1;
    }
    g_handler_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_handler_ready_event) {
        SENTRY_WARN("failed to create handler ready event");
        CloseHandle(g_handler_semaphore);
        g_handler_semaphore = NULL;
        CloseHandle(g_handler_ack_semaphore);
        g_handler_ack_semaphore = NULL;
        return 1;
    }
#endif
    return 0;
}

static void
teardown_handler_sync(void)
{
#ifdef SENTRY_PLATFORM_UNIX
    close_pipe(g_handler_pipe);
    close_pipe(g_handler_ack_pipe);
    close_pipe(g_handler_ready_pipe);
#elif defined(SENTRY_PLATFORM_WINDOWS)
    if (g_handler_semaphore) {
        CloseHandle(g_handler_semaphore);
        g_handler_semaphore = NULL;
    }
    if (g_handler_ack_semaphore) {
        CloseHandle(g_handler_ack_semaphore);
        g_handler_ack_semaphore = NULL;
    }
    if (g_handler_ready_event) {
        CloseHandle(g_handler_ready_event);
        g_handler_ready_event = NULL;
    }
#endif
}

// Signals that the handler thread is ready to process crashes
static void
signal_handler_ready(void)
{
    sentry__atomic_store(&g_handler_thread_ready, 1);
#ifdef SENTRY_PLATFORM_UNIX
    char c = 1;
    ssize_t rv;
    do {
        rv = write(g_handler_ready_pipe[1], &c, 1);
    } while (rv == -1 && errno == EINTR);
    close(g_handler_ready_pipe[1]);
    g_handler_ready_pipe[1] = -1;
#elif defined(SENTRY_PLATFORM_WINDOWS)
    SetEvent(g_handler_ready_event);
#endif
}

// Allows init to wait until the handler thread is ready to handle crashes. The
// initialization is quick enough for early crashes ending up in a half-
// initialized handler state.
//
// A timed out handler thread is an initialization error. The wait happens with
// OS-level polling rather than spinning on an atomic.
static bool
wait_for_handler_ready(int timeout_ms)
{
#ifdef SENTRY_PLATFORM_UNIX
    struct pollfd pfd;
    pfd.fd = g_handler_ready_pipe[0];
    pfd.events = POLLIN;
    int rv;
    do {
        rv = poll(&pfd, 1, timeout_ms);
    } while (rv == -1 && errno == EINTR);
    if (rv == -1) {
        SENTRY_WARNF("poll for handler ready failed: %s", strerror(errno));
    } else if (rv > 0) {
        char c;
        read(g_handler_ready_pipe[0], &c, 1);
    }
#elif defined(SENTRY_PLATFORM_WINDOWS)
    WaitForSingleObject(g_handler_ready_event, (DWORD)timeout_ms);
#endif
    return sentry__atomic_fetch(&g_handler_thread_ready);
}

// Wakes the handler thread. If `close_for_exit` is true, closes the write end
// of the pipe (Unix) to cause an EOF, otherwise writes a command byte.
static void
wake_handler_thread(bool close_for_exit)
{
#ifdef SENTRY_PLATFORM_UNIX
    if (g_handler_pipe[1] >= 0) {
        if (close_for_exit) {
            close(g_handler_pipe[1]);
            g_handler_pipe[1] = -1;
        } else {
            char c = 0;
            ssize_t rv;
            do {
                rv = write(g_handler_pipe[1], &c, 1);
            } while (rv == -1 && errno == EINTR);
        }
    }
#elif defined(SENTRY_PLATFORM_WINDOWS)
    (void)close_for_exit;
    if (g_handler_semaphore) {
        ReleaseSemaphore(g_handler_semaphore, 1, NULL);
    }
#endif
}

static int start_handler_thread(void);
static void stop_handler_thread(void);

#ifdef SENTRY_PLATFORM_UNIX
#    include <unistd.h>
struct signal_slot {
    int signum;
    const char *signame;
    const char *sigdesc;
};

// we need quite a bit of space for backtrace generation
#    define SIGNAL_COUNT 6
#    define SIGNAL_STACK_SIZE (1024 * SENTRY_HANDLER_STACK_SIZE)
static struct sigaction g_sigaction;
static struct sigaction g_previous_handlers[SIGNAL_COUNT];
static stack_t g_signal_stack = { 0 };
static const struct signal_slot SIGNAL_DEFINITIONS[SIGNAL_COUNT] = {
    SIGNAL_DEF(SIGILL, "IllegalInstruction"),
    SIGNAL_DEF(SIGTRAP, "Trap"),
    SIGNAL_DEF(SIGABRT, "Abort"),
    SIGNAL_DEF(SIGBUS, "BusError"),
    SIGNAL_DEF(SIGFPE, "FloatingPointException"),
    SIGNAL_DEF(SIGSEGV, "Segfault"),
};

static void handle_signal(int signum, siginfo_t *info, void *user_context);

/**
 * Sets up an alternate signal stack for the current thread if one isn't
 * already configured. The allocated stack is stored in `out_stack` so it
 * can be freed later via `teardown_sigaltstack()`.
 */
static void
setup_sigaltstack(stack_t *out_stack, const char *context)
{
    memset(out_stack, 0, sizeof(*out_stack));

    stack_t old_sig_stack;
    int ret = sigaltstack(NULL, &old_sig_stack);
    if (ret == 0 && old_sig_stack.ss_flags == SS_DISABLE) {
        SENTRY_DEBUGF(
            "installing %s sigaltstack (size: %d)", context, SIGNAL_STACK_SIZE);
        out_stack->ss_sp = sentry_malloc(SIGNAL_STACK_SIZE);
        if (!out_stack->ss_sp) {
            SENTRY_WARN("failed to allocate signal stack");
            return;
        }
        out_stack->ss_size = SIGNAL_STACK_SIZE;
        out_stack->ss_flags = 0;
        sigaltstack(out_stack, 0);
    } else if (ret == 0) {
        SENTRY_DEBUGF("using existing signal stack (size: %d, flags: %d)",
            old_sig_stack.ss_size, old_sig_stack.ss_flags);
    } else if (ret == -1) {
        SENTRY_WARNF("failed to query signal stack: %s", strerror(errno));
    }
}

/**
 * Tears down a signal stack previously set up via `setup_sigaltstack()`.
 */
static void
teardown_sigaltstack(stack_t *sig_stack)
{
    if (sig_stack->ss_sp) {
        sig_stack->ss_flags = SS_DISABLE;
        sigaltstack(sig_stack, 0);
        sentry_free(sig_stack->ss_sp);
        sig_stack->ss_sp = NULL;
    }
}

static void
reset_signal_handlers(void)
{
    for (size_t i = 0; i < SIGNAL_COUNT; i++) {
        sigaction(SIGNAL_DEFINITIONS[i].signum, &g_previous_handlers[i], 0);
    }
}

static void
invoke_signal_handler(int signum, siginfo_t *info, void *user_context)
{
    for (int i = 0; i < SIGNAL_COUNT; ++i) {
        if (SIGNAL_DEFINITIONS[i].signum == signum) {
            struct sigaction *handler = &g_previous_handlers[i];
            if (handler->sa_handler == SIG_DFL) {
                raise(signum);
            } else if (handler->sa_flags & SA_SIGINFO) {
                handler->sa_sigaction(signum, info, user_context);
            } else if (handler->sa_handler != SIG_IGN) {
                // This handler can only handle to signal number (ANSI C)
                void (*func)(int) = handler->sa_handler;
                func(signum);
            }
        }
    }
}

static int
install_signal_handlers(void)
{
    if (sentry__atomic_fetch(&g_preloaded)) {
        return 0;
    }

    memset(g_previous_handlers, 0, sizeof(g_previous_handlers));
    for (size_t i = 0; i < SIGNAL_COUNT; ++i) {
        if (sigaction(
                SIGNAL_DEFINITIONS[i].signum, NULL, &g_previous_handlers[i])
            == -1) {
            return 1;
        }
    }

    setup_sigaltstack(&g_signal_stack, "init");

    sigemptyset(&g_sigaction.sa_mask);
    g_sigaction.sa_sigaction = handle_signal;
    // SA_NODEFER allows the signal to be delivered while the handler is
    // running. This is needed for recursive crash detection to work -
    // without it, a crash during crash handling would block the signal
    // and leave the process in an undefined state.
    g_sigaction.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    for (size_t i = 0; i < SIGNAL_COUNT; ++i) {
        sigaction(SIGNAL_DEFINITIONS[i].signum, &g_sigaction, NULL);
    }
    return 0;
}

static int
startup_inproc_backend(
    sentry_backend_t *backend, const sentry_options_t *options)
{
#    ifdef SENTRY_WITH_UNWINDER_LIBBACKTRACE
    SENTRY_WARN("Using `backtrace()` for stack traces together with the inproc "
                "backend is signal-unsafe. This is a fallback configuration.");
#    endif
    // get option state so we don't need to sync read during signal handling
    g_backend_config.enable_logging_when_crashed
        = options ? options->enable_logging_when_crashed : true;
    g_backend_config.handler_strategy =
#    if defined(SENTRY_PLATFORM_LINUX)
        options ? sentry_options_get_handler_strategy(options) :
#    endif
                SENTRY_HANDLER_STRATEGY_DEFAULT;
#    ifdef SENTRY_PLATFORM_ANDROID
    // CHAIN_AT_START invokes the previous handler and expects to regain
    // control. On Android API < 26, the old debuggerd daemon kills the
    // crashing process via SIGKILL after the chained handler triggers
    // it, so we fall back to DEFAULT which chains at the end instead.
    if (g_backend_config.handler_strategy
            == SENTRY_HANDLER_STRATEGY_CHAIN_AT_START
        && android_get_device_api_level() < 26) {
        g_backend_config.handler_strategy = SENTRY_HANDLER_STRATEGY_DEFAULT;
    }
#    endif
    if (backend) {
        backend->data = &g_backend_config;
    }

    if (start_handler_thread() != 0) {
        return 1;
    }

    return install_signal_handlers();
}

static void
shutdown_inproc_backend(sentry_backend_t *backend)
{
    stop_handler_thread();

    // In Android preload mode, we intentionally keep our signal handlers
    // installed across shutdown. The handler thread is torn down, but our
    // position in the signal chain must remain stable so that a later
    // sentry_init() can reactivate full crash handling without losing the
    // ordering relative to the managed runtime.
    //
    // While in this state, full inproc crash processing is inactive and any
    // signal we still observe will fall through to the previously installed
    // handler.
    if (!sentry__atomic_fetch(&g_preloaded)) {
        teardown_sigaltstack(&g_signal_stack);
        reset_signal_handlers();
    }

    if (backend) {
        backend->data = NULL;
    }

    // allow tests or orderly shutdown to re-arm the backend once unregistered
    sentry__atomic_store(&g_crash_handling_state, CRASH_STATE_IDLE);
}

#elif defined(SENTRY_PLATFORM_WINDOWS)

struct signal_slot {
    DWORD signum;
    const char *signame;
    const char *sigdesc;
};

#    define SIGNAL_COUNT 21

static LPTOP_LEVEL_EXCEPTION_FILTER g_previous_handler = NULL;

static const struct signal_slot SIGNAL_DEFINITIONS[SIGNAL_COUNT] = {
    SIGNAL_DEF(EXCEPTION_ACCESS_VIOLATION, "AccessViolation"),
    SIGNAL_DEF(EXCEPTION_ARRAY_BOUNDS_EXCEEDED, "ArrayBoundsExceeded"),
    SIGNAL_DEF(EXCEPTION_BREAKPOINT, "BreakPoint"),
    SIGNAL_DEF(EXCEPTION_DATATYPE_MISALIGNMENT, "DatatypeMisalignment"),
    SIGNAL_DEF(EXCEPTION_FLT_DENORMAL_OPERAND, "FloatDenormalOperand"),
    SIGNAL_DEF(EXCEPTION_FLT_DIVIDE_BY_ZERO, "FloatDivideByZero"),
    SIGNAL_DEF(EXCEPTION_FLT_INEXACT_RESULT, "FloatInexactResult"),
    SIGNAL_DEF(EXCEPTION_FLT_INVALID_OPERATION, "FloatInvalidOperation"),
    SIGNAL_DEF(EXCEPTION_FLT_OVERFLOW, "FloatOverflow"),
    SIGNAL_DEF(EXCEPTION_FLT_STACK_CHECK, "FloatStackCheck"),
    SIGNAL_DEF(EXCEPTION_FLT_UNDERFLOW, "FloatUnderflow"),
    SIGNAL_DEF(EXCEPTION_ILLEGAL_INSTRUCTION, "IllegalInstruction"),
    SIGNAL_DEF(EXCEPTION_IN_PAGE_ERROR, "InPageError"),
    SIGNAL_DEF(EXCEPTION_INT_DIVIDE_BY_ZERO, "IntegerDivideByZero"),
    SIGNAL_DEF(EXCEPTION_INT_OVERFLOW, "IntegerOverflow"),
    SIGNAL_DEF(EXCEPTION_INVALID_DISPOSITION, "InvalidDisposition"),
    SIGNAL_DEF(EXCEPTION_NONCONTINUABLE_EXCEPTION, "NonContinuableException"),
    SIGNAL_DEF(EXCEPTION_PRIV_INSTRUCTION, "PrivilgedInstruction"),
    SIGNAL_DEF(EXCEPTION_SINGLE_STEP, "SingleStep"),
    SIGNAL_DEF(EXCEPTION_STACK_OVERFLOW, "StackOverflow"),
    SIGNAL_DEF(STATUS_FATAL_APP_EXIT, "FatalAppExit"),
};

static LONG WINAPI handle_exception(EXCEPTION_POINTERS *);

static void
handle_sigabrt(EXCEPTION_POINTERS *exception_pointers)
{
    handle_exception(exception_pointers);
}

static int
startup_inproc_backend(
    sentry_backend_t *backend, const sentry_options_t *options)
{
    g_backend_config.enable_logging_when_crashed
        = options ? options->enable_logging_when_crashed : true;
    g_backend_config.handler_strategy = SENTRY_HANDLER_STRATEGY_DEFAULT;
    if (backend) {
        backend->data = &g_backend_config;
    }

#    if !defined(SENTRY_BUILD_SHARED)                                          \
        && defined(SENTRY_THREAD_STACK_GUARANTEE_AUTO_INIT)
    sentry__set_default_thread_stack_guarantee();
#    endif
    if (start_handler_thread() != 0) {
        return 1;
    }
    g_previous_handler = SetUnhandledExceptionFilter(&handle_exception);
    SetErrorMode(SEM_FAILCRITICALERRORS);

    sentry__win32_install_sigabrt_handler(handle_sigabrt);

    return 0;
}

static void
shutdown_inproc_backend(sentry_backend_t *backend)
{
    stop_handler_thread();

    LPTOP_LEVEL_EXCEPTION_FILTER current_handler
        = SetUnhandledExceptionFilter(g_previous_handler);
    if (current_handler != &handle_exception) {
        SetUnhandledExceptionFilter(current_handler);
    }

    sentry__win32_restore_sigabrt_handler();

    if (backend) {
        backend->data = NULL;
    }

    // the inproc handler is now unregistered; re-arm the guard for future use
    sentry__atomic_store(&g_crash_handling_state, CRASH_STATE_IDLE);
}

#endif

static sentry_value_t
registers_from_uctx(const sentry_ucontext_t *uctx)
{
    sentry_value_t registers = sentry_value_new_object();

#if defined(SENTRY_PLATFORM_LINUX)

    // just assume the ctx is a bunch of uintpr_t, and index that directly
    uintptr_t *ctx = (uintptr_t *)&uctx->user_context->uc_mcontext;

#    define SET_REG(name, num)                                                 \
        sentry_value_set_by_key(registers, name,                               \
            sentry__value_new_addr((uint64_t)(size_t)ctx[num]));

#    if defined(__x86_64__)

    SET_REG("r8", 0);
    SET_REG("r9", 1);
    SET_REG("r10", 2);
    SET_REG("r11", 3);
    SET_REG("r12", 4);
    SET_REG("r13", 5);
    SET_REG("r14", 6);
    SET_REG("r15", 7);
    SET_REG("rdi", 8);
    SET_REG("rsi", 9);
    SET_REG("rbp", 10);
    SET_REG("rbx", 11);
    SET_REG("rdx", 12);
    SET_REG("rax", 13);
    SET_REG("rcx", 14);
    SET_REG("rsp", 15);
    SET_REG("rip", 16);

#    elif defined(__i386__)

    // gs, fs, es, ds
    SET_REG("edi", 4);
    SET_REG("esi", 5);
    SET_REG("ebp", 6);
    SET_REG("esp", 7);
    SET_REG("ebx", 8);
    SET_REG("edx", 9);
    SET_REG("ecx", 10);
    SET_REG("eax", 11);
    SET_REG("eip", 14);
    SET_REG("eflags", 16);

#    elif defined(__aarch64__)

    // 0 is `fault_address`
    SET_REG("x0", 1);
    SET_REG("x1", 2);
    SET_REG("x2", 3);
    SET_REG("x3", 4);
    SET_REG("x4", 5);
    SET_REG("x5", 6);
    SET_REG("x6", 7);
    SET_REG("x7", 8);
    SET_REG("x8", 9);
    SET_REG("x9", 10);
    SET_REG("x10", 11);
    SET_REG("x11", 12);
    SET_REG("x12", 13);
    SET_REG("x13", 14);
    SET_REG("x14", 15);
    SET_REG("x15", 16);
    SET_REG("x16", 17);
    SET_REG("x17", 18);
    SET_REG("x18", 19);
    SET_REG("x19", 20);
    SET_REG("x20", 21);
    SET_REG("x21", 22);
    SET_REG("x22", 23);
    SET_REG("x23", 24);
    SET_REG("x24", 25);
    SET_REG("x25", 26);
    SET_REG("x26", 27);
    SET_REG("x27", 28);
    SET_REG("x28", 29);
    SET_REG("fp", 30);
    SET_REG("lr", 31);
    SET_REG("sp", 32);
    SET_REG("pc", 33);

#    elif defined(__arm__)

    // trap_no, _error_code, oldmask
    SET_REG("r0", 3);
    SET_REG("r1", 4);
    SET_REG("r2", 5);
    SET_REG("r3", 6);
    SET_REG("r4", 7);
    SET_REG("r5", 8);
    SET_REG("r6", 9);
    SET_REG("r7", 10);
    SET_REG("r8", 11);
    SET_REG("r9", 12);
    SET_REG("r10", 13);
    SET_REG("fp", 14);
    SET_REG("ip", 15);
    SET_REG("sp", 16);
    SET_REG("lr", 17);
    SET_REG("pc", 18);

#    endif

#    undef SET_REG

#elif defined(SENTRY_PLATFORM_DARWIN)

#    define SET_REG(name, prop)                                                \
        sentry_value_set_by_key(registers, name,                               \
            sentry__value_new_addr((uint64_t)(size_t)thread_state->prop));

#    if defined(__x86_64__)

    _STRUCT_X86_THREAD_STATE64 *thread_state
        = &uctx->user_context->uc_mcontext->__ss;

    SET_REG("rax", __rax);
    SET_REG("rbx", __rbx);
    SET_REG("rcx", __rcx);
    SET_REG("rdx", __rdx);
    SET_REG("rdi", __rdi);
    SET_REG("rsi", __rsi);
    SET_REG("rbp", __rbp);
    SET_REG("rsp", __rsp);
    SET_REG("r8", __r8);
    SET_REG("r9", __r9);
    SET_REG("r10", __r10);
    SET_REG("r11", __r11);
    SET_REG("r12", __r12);
    SET_REG("r13", __r13);
    SET_REG("r14", __r14);
    SET_REG("r15", __r15);
    SET_REG("rip", __rip);

#    elif defined(__arm64__)

    _STRUCT_ARM_THREAD_STATE64 *thread_state
        = &uctx->user_context->uc_mcontext->__ss;

    SET_REG("x0", __x[0]);
    SET_REG("x1", __x[1]);
    SET_REG("x2", __x[2]);
    SET_REG("x3", __x[3]);
    SET_REG("x4", __x[4]);
    SET_REG("x5", __x[5]);
    SET_REG("x6", __x[6]);
    SET_REG("x7", __x[7]);
    SET_REG("x8", __x[8]);
    SET_REG("x9", __x[9]);
    SET_REG("x10", __x[10]);
    SET_REG("x11", __x[11]);
    SET_REG("x12", __x[12]);
    SET_REG("x13", __x[13]);
    SET_REG("x14", __x[14]);
    SET_REG("x15", __x[15]);
    SET_REG("x16", __x[16]);
    SET_REG("x17", __x[17]);
    SET_REG("x18", __x[18]);
    SET_REG("x19", __x[19]);
    SET_REG("x20", __x[20]);
    SET_REG("x21", __x[21]);
    SET_REG("x22", __x[22]);
    SET_REG("x23", __x[23]);
    SET_REG("x24", __x[24]);
    SET_REG("x25", __x[25]);
    SET_REG("x26", __x[26]);
    SET_REG("x27", __x[27]);
    SET_REG("x28", __x[28]);
#        if defined(__arm64e__)
    sentry_value_set_by_key(registers, "fp",
        sentry__value_new_addr(
            (uint64_t)__darwin_arm_thread_state64_get_fp(*thread_state)));
    sentry_value_set_by_key(registers, "lr",
        sentry__value_new_addr(
            (uint64_t)__darwin_arm_thread_state64_get_lr(*thread_state)));
    sentry_value_set_by_key(registers, "sp",
        sentry__value_new_addr(
            (uint64_t)__darwin_arm_thread_state64_get_sp(*thread_state)));
    sentry_value_set_by_key(registers, "pc",
        sentry__value_new_addr(
            (uint64_t)__darwin_arm_thread_state64_get_pc(*thread_state)));
#        else
    SET_REG("fp", __fp);
    SET_REG("lr", __lr);
    SET_REG("sp", __sp);
    SET_REG("pc", __pc);
#        endif

#    elif defined(__arm__)

    _STRUCT_ARM_THREAD_STATE *thread_state
        = &uctx->user_context->uc_mcontext->__ss;

    SET_REG("r0", __r[0]);
    SET_REG("r1", __r[1]);
    SET_REG("r2", __r[2]);
    SET_REG("r3", __r[3]);
    SET_REG("r4", __r[4]);
    SET_REG("r5", __r[5]);
    SET_REG("r6", __r[6]);
    SET_REG("r7", __r[7]);
    SET_REG("r8", __r[8]);
    SET_REG("r9", __r[9]);
    SET_REG("r10", __r[10]);
    SET_REG("fp", __r[11]);
    SET_REG("ip", __r[12]);
    SET_REG("sp", __sp);
    SET_REG("lr", __lr);
    SET_REG("pc", __pc);

#    endif

#    undef SET_REG

#elif defined(SENTRY_PLATFORM_WINDOWS)
    PCONTEXT ctx = uctx->exception_ptrs.ContextRecord;

#    define SET_REG(name, prop)                                                \
        sentry_value_set_by_key(registers, name,                               \
            sentry__value_new_addr((uint64_t)(size_t)ctx->prop))

#    if defined(_M_AMD64)

    if (ctx->ContextFlags & CONTEXT_INTEGER) {
        SET_REG("rax", Rax);
        SET_REG("rcx", Rcx);
        SET_REG("rdx", Rdx);
        SET_REG("rbx", Rbx);
        SET_REG("rbp", Rbp);
        SET_REG("rsi", Rsi);
        SET_REG("rdi", Rdi);
        SET_REG("r8", R8);
        SET_REG("r9", R9);
        SET_REG("r10", R10);
        SET_REG("r11", R11);
        SET_REG("r12", R12);
        SET_REG("r13", R13);
        SET_REG("r14", R14);
        SET_REG("r15", R15);
    }

    if (ctx->ContextFlags & CONTEXT_CONTROL) {
        SET_REG("rsp", Rsp);
        SET_REG("rip", Rip);
    }

#    elif defined(_M_IX86)

    if (ctx->ContextFlags & CONTEXT_INTEGER) {
        SET_REG("edi", Edi);
        SET_REG("esi", Esi);
        SET_REG("ebx", Ebx);
        SET_REG("edx", Edx);
        SET_REG("ecx", Ecx);
        SET_REG("eax", Eax);
    }

    if (ctx->ContextFlags & CONTEXT_CONTROL) {
        SET_REG("ebp", Ebp);
        SET_REG("eip", Eip);
        SET_REG("eflags", EFlags);
        SET_REG("esp", Esp);
    }

#    elif defined(_M_ARM64)

    if (ctx->ContextFlags & CONTEXT_INTEGER) {
        SET_REG("x0", X0);
        SET_REG("x1", X1);
        SET_REG("x2", X2);
        SET_REG("x3", X3);
        SET_REG("x4", X4);
        SET_REG("x5", X5);
        SET_REG("x6", X6);
        SET_REG("x7", X7);
        SET_REG("x8", X8);
        SET_REG("x9", X9);
        SET_REG("x10", X10);
        SET_REG("x11", X11);
        SET_REG("x12", X12);
        SET_REG("x13", X13);
        SET_REG("x14", X14);
        SET_REG("x15", X15);
        SET_REG("x16", X16);
        SET_REG("x17", X17);
        // x18 is reserved as platform register on Windows
        SET_REG("x19", X19);
        SET_REG("x20", X20);
        SET_REG("x21", X21);
        SET_REG("x22", X22);
        SET_REG("x23", X23);
        SET_REG("x24", X24);
        SET_REG("x25", X25);
        SET_REG("x26", X26);
        SET_REG("x27", X27);
        SET_REG("x28", X28);
    }

    if (ctx->ContextFlags & CONTEXT_CONTROL) {
        SET_REG("fp", Fp);
        SET_REG("lr", Lr);
        SET_REG("sp", Sp);
        SET_REG("pc", Pc);
    }

#    endif

#    undef SET_REG

#endif

    return registers;
}

#ifdef SENTRY_PLATFORM_LINUX
static uintptr_t
get_stack_pointer(const sentry_ucontext_t *uctx)
{
#    if defined(__i386__)
    return uctx->user_context->uc_mcontext.gregs[REG_ESP];
#    elif defined(__x86_64__)
    return uctx->user_context->uc_mcontext.gregs[REG_RSP];
#    elif defined(__arm__)
    return uctx->user_context->uc_mcontext.arm_sp;
#    elif defined(__aarch64__)
    return uctx->user_context->uc_mcontext.sp;
#    else
    SENTRY_WARN("get_stack_pointer is not implemented for this architecture. "
                "Signal chaining may not work as expected.");
    return NULL;
#    endif
}

static uintptr_t
get_instruction_pointer(const sentry_ucontext_t *uctx)
{
#    if defined(__i386__)
    return uctx->user_context->uc_mcontext.gregs[REG_EIP];
#    elif defined(__x86_64__)
    return uctx->user_context->uc_mcontext.gregs[REG_RIP];
#    elif defined(__arm__)
    return uctx->user_context->uc_mcontext.arm_pc;
#    elif defined(__aarch64__)
    return uctx->user_context->uc_mcontext.pc;
#    else
    SENTRY_WARN(
        "get_instruction_pointer is not implemented for this architecture. "
        "Signal chaining may not work as expected.");
    return NULL;
#    endif
}
#endif

static sentry_value_t
make_signal_event(const struct signal_slot *sig_slot,
    const sentry_ucontext_t *uctx, sentry_handler_strategy_t strategy)
{
    sentry_value_t event = sentry_value_new_event();
    sentry_value_set_by_key(
        event, "level", sentry__value_new_level(SENTRY_LEVEL_FATAL));

    sentry_value_t exc = sentry_value_new_exception(
        sig_slot ? sig_slot->signame : "UNKNOWN_SIGNAL",
        sig_slot ? sig_slot->sigdesc : "UnknownSignal");

    sentry_value_t mechanism = sentry_value_new_object();
    sentry_value_set_by_key(exc, "mechanism", mechanism);

    sentry_value_t mechanism_meta = sentry_value_new_object();
    sentry_value_t signal_meta = sentry_value_new_object();
    if (sig_slot) {
        sentry_value_set_by_key(
            signal_meta, "name", sentry_value_new_string(sig_slot->signame));
        // relay interprets the signal number as an i64:
        // https://github.com/getsentry/relay/blob/e96e4b037cfddaa7b0fb97a0909d100dde034f8e/relay-event-schema/src/protocol/mechanism.rs#L52-L53
        // This covers the signal number ranges of all supported platforms.
        sentry_value_set_by_key(
            signal_meta, "number", sentry_value_new_int64(sig_slot->signum));
    }
    sentry_value_set_by_key(mechanism_meta, "signal", signal_meta);
    sentry_value_set_by_key(
        mechanism, "type", sentry_value_new_string("signalhandler"));
    sentry_value_set_by_key(
        mechanism, "synthetic", sentry_value_new_bool(true));
    sentry_value_set_by_key(mechanism, "handled", sentry_value_new_bool(false));
    sentry_value_set_by_key(mechanism, "meta", mechanism_meta);

    void *backtrace[MAX_FRAMES];
    size_t frame_count
        = sentry_unwind_stack_from_ucontext(uctx, &backtrace[0], MAX_FRAMES);
    SENTRY_DEBUGF(
        "captured backtrace from ucontext with %lu frames", frame_count);
    // if unwinding from a ucontext didn't yield any results, try again with a
    // direct unwind. this is most likely the case when using `libbacktrace`,
    // since that does not allow to unwind from a ucontext at all. the fallback
    // is skipped with the "chain at start" strategy because `libbacktrace`
    // crashes, and would likely not provide helpful information anyway.
    if (!frame_count && strategy != SENTRY_HANDLER_STRATEGY_CHAIN_AT_START) {
        frame_count = sentry_unwind_stack(NULL, &backtrace[0], MAX_FRAMES);
    }
    SENTRY_DEBUGF("captured backtrace with %lu frames", frame_count);

    sentry_value_t stacktrace
        = sentry_value_new_stacktrace(&backtrace[0], frame_count);

    sentry_value_t registers = registers_from_uctx(uctx);
    sentry_value_set_by_key(stacktrace, "registers", registers);

#ifdef SENTRY_WITH_UNWINDER_LIBUNWINDSTACK
    // libunwindstack already adjusts the PC according to `GetPcAdjustment()`
    // https://github.com/getsentry/libunwindstack-ndk/blob/1929f7b601797fc8b2cac092d563b31d01d46a76/Regs.cpp#L187
    // so there is no need to adjust the PC in the backend processing.
    sentry_value_set_by_key(stacktrace, "instruction_addr_adjustment",
        sentry_value_new_string("none"));
#endif

    sentry_value_set_by_key(exc, "stacktrace", stacktrace);
    sentry_event_add_exception(event, exc);

    return event;
}

/**
 * This is the signal-unsafe part of the inproc handler. Everything that
 * requires stdio, time-formatting/-capture or serialization must happen here.
 *
 * Although we can use signal-unsafe functions here, this should still be
 * written with care. Don't rely on thread synchronization or the system
 * allocator since the program is in a crashed state. At least one thread no
 * longer progresses and memory can be corrupted.
 */

// Test hook for triggering crashes at specific points in the handler.
// Set via sentry__set_test_crash_callback() during testing.
#ifdef SENTRY_UNITTEST
typedef void (*sentry_test_crash_callback_t)(const char *location);
static sentry_test_crash_callback_t g_test_crash_callback = NULL;

void
sentry__set_test_crash_callback(sentry_test_crash_callback_t callback)
{
    g_test_crash_callback = callback;
}

#    define TEST_CRASH_POINT(location)                                         \
        do {                                                                   \
            if (g_test_crash_callback) {                                       \
                g_test_crash_callback(location);                               \
            }                                                                  \
        } while (0)
#else
#    define TEST_CRASH_POINT(location) ((void)0)
#endif

static void
process_ucontext_deferred(const sentry_ucontext_t *uctx,
    const struct signal_slot *sig_slot, bool skip_hooks)
{
    SENTRY_INFO("entering signal handler");
    TEST_CRASH_POINT("after_enter");

    SENTRY_WITH_OPTIONS (options) {
        sentry_handler_strategy_t strategy =
#if defined(SENTRY_PLATFORM_LINUX)
            options ? sentry_options_get_handler_strategy(options) :
#endif
                    SENTRY_HANDLER_STRATEGY_DEFAULT;
        sentry_value_t event = make_signal_event(sig_slot, uctx, strategy);
        bool should_handle = true;
        sentry__write_crash_marker(options);

        sentry_value_t transaction
            = sentry__trace_finish(SENTRY_SPAN_STATUS_ABORTED);

        if (options->on_crash_func && !skip_hooks) {
            SENTRY_DEBUG("invoking `on_crash` hook");
            event = options->on_crash_func(uctx, event, options->on_crash_data);
            should_handle = !sentry_value_is_null(event);
        } else if (skip_hooks && options->on_crash_func) {
            SENTRY_DEBUG("skipping `on_crash` hook due to recursive crash");
        }

        // Flush logs in a crash-safe manner before crash handling
        if (options->enable_logs) {
            sentry__logs_flush_crash_safe();
        }
        if (options->enable_metrics) {
            sentry__metrics_flush_crash_safe();
        }
        TEST_CRASH_POINT("before_capture");
        if (should_handle) {
            bool capture_screenshot = options->attach_screenshot;
#ifndef SENTRY_SCREENSHOT_NONE
            if (capture_screenshot && options->before_screenshot_func) {
                SENTRY_DEBUG("invoking `before_screenshot` hook");
                capture_screenshot = options->before_screenshot_func(
                                         event, options->before_screenshot_data)
                    != 0;
            }
#endif

            sentry_envelope_t *envelope = sentry__prepare_event(options, event,
                NULL, !options->on_crash_func && !skip_hooks, NULL);
            sentry_session_t *session = sentry__end_current_session_with_status(
                SENTRY_SESSION_STATUS_CRASHED);
            sentry__envelope_add_session(envelope, session);

            if (capture_screenshot) {
                sentry_attachment_t *screenshot = sentry__attachment_from_path(
                    sentry__screenshot_get_path(options));
                if (screenshot
                    && sentry__screenshot_capture(screenshot->path, 0)) {
                    sentry__envelope_add_attachment(envelope, screenshot);
                }
                sentry__attachment_free(screenshot);
            }

            if (options->attach_session_replay) {
                sentry_attachment_t *replay = sentry__attachment_from_path(
                    sentry__session_replay_get_path(options));
                if (replay
                    && sentry__session_replay_capture(
                        replay->path, options->session_replay_duration, 0)) {
                    sentry__envelope_add_attachment(envelope, replay);
                }
                sentry__attachment_free(replay);
            }

            if (envelope && sentry__session_replay_has_pending(options)) {
                sentry_value_t crash_event
                    = sentry_envelope_get_event(envelope);
                sentry_transport_t *replay_transport
                    = sentry_new_disk_transport(options->run);
                if (replay_transport) {
                    sentry__session_replay_flush_pending(
                        options, replay_transport, crash_event);
                    sentry__transport_dump_queue(
                        replay_transport, options->run);
                    sentry_transport_free(replay_transport);
                }
            }

            if (!sentry__launch_external_crash_reporter(options, envelope)) {
                // capture the envelopes with the disk transport
                sentry_transport_t *disk_transport
                    = sentry_new_disk_transport(options->run);
                if (!sentry_value_is_null(transaction)) {
                    sentry_envelope_t *tx_envelope
                        = sentry__prepare_transaction(
                            options, transaction, NULL);
                    if (tx_envelope) {
                        sentry__capture_envelope(
                            disk_transport, tx_envelope, options);
                    }
                }
                sentry__capture_envelope(disk_transport, envelope, options);
                sentry__transport_dump_queue(disk_transport, options->run);
                sentry_transport_free(disk_transport);
            } else {
                sentry_value_decref(transaction);
            }
        } else {
            SENTRY_DEBUG("event was discarded by the `on_crash` hook");
            sentry_value_decref(event);
            sentry_value_decref(transaction);
        }

        // after capturing the crash event, dump all the envelopes to disk
        sentry__transport_dump_queue(options->transport, options->run);

        // Use signal-safe logging here since this may run in signal handler
        // context (fallback path) where stdio functions are not safe.
        SENTRY_SIGNAL_SAFE_LOG("crash has been captured");
    }
}

SENTRY_THREAD_FN
handler_thread_main(void *UNUSED(data))
{
#ifdef SENTRY_PLATFORM_UNIX
    // Set up an alternate signal stack for the handler thread
    stack_t handler_thread_stack;
    setup_sigaltstack(&handler_thread_stack, "handler");
#endif

    signal_handler_ready();

    for (;;) {
#ifdef SENTRY_PLATFORM_UNIX
        char command = 0;
        ssize_t rv = read(g_handler_pipe[0], &command, 1);
        if (rv == -1 && errno == EINTR) {
            continue;
        }
        if (rv <= 0) {
            if (sentry__atomic_fetch(&g_handler_should_exit)) {
                break;
            }
            continue;
        }
#elif defined(SENTRY_PLATFORM_WINDOWS)
        DWORD wait_result = WaitForSingleObject(g_handler_semaphore, INFINITE);
        if (wait_result != WAIT_OBJECT_0) {
            continue;
        }
#endif

        if (!sentry__atomic_fetch(&g_handler_has_work)) {
            if (sentry__atomic_fetch(&g_handler_should_exit)) {
                break;
            }
            continue;
        }

        process_ucontext_deferred(
            &g_handler_state.uctx, g_handler_state.sig_slot, false);
        sentry__atomic_store(&g_handler_has_work, 0);
#ifdef SENTRY_PLATFORM_UNIX
        if (g_handler_ack_pipe[1] >= 0) {
            char c = 1;
            do {
                rv = write(g_handler_ack_pipe[1], &c, 1);
            } while (rv == -1 && errno == EINTR);
            if (rv != 1) {
                SENTRY_SIGNAL_SAFE_LOG("WARN failed to write handler ack");
                close(g_handler_ack_pipe[1]);
                g_handler_ack_pipe[1] = -1;
                sentry__atomic_store(&g_handler_should_exit, 1);
                break;
            }
        }
#elif defined(SENTRY_PLATFORM_WINDOWS)
        if (g_handler_ack_semaphore) {
            ReleaseSemaphore(g_handler_ack_semaphore, 1, NULL);
        }
#endif

        if (sentry__atomic_fetch(&g_handler_should_exit)) {
            break;
        }
    }

#ifdef SENTRY_PLATFORM_UNIX
    teardown_sigaltstack(&handler_thread_stack);
#endif

#ifdef SENTRY_PLATFORM_WINDOWS
    return 0;
#else
    return NULL;
#endif
}

static int
start_handler_thread(void)
{
    if (sentry__atomic_fetch(&g_handler_thread_ready)) {
        return 0;
    }

    sentry__thread_init(&g_handler_thread);
    sentry__atomic_store(&g_handler_should_exit, 0);
    sentry__atomic_store(&g_handler_has_work, 0);

    if (init_handler_sync() != 0) {
        return 1;
    }

    if (sentry__thread_spawn(&g_handler_thread, handler_thread_main, NULL)
        != 0) {
        SENTRY_WARN("failed to spawn handler thread");
        teardown_handler_sync();
        return 1;
    }

    if (!wait_for_handler_ready(5000)) {
        SENTRY_WARN("handler thread failed to start in time");
        // Thread was spawned but didn't become ready. Signal it to exit,
        // join it, and clean up all resources.
        sentry__atomic_store(&g_handler_should_exit, 1);
        wake_handler_thread(true);
        sentry__thread_join(g_handler_thread);
        sentry__thread_free(&g_handler_thread);

        // The thread may have set g_handler_thread_ready before exiting;
        // ensure it's cleared so we don't appear "started".
        sentry__atomic_store(&g_handler_thread_ready, 0);
        teardown_handler_sync();
        return 1;
    }

    return 0;
}

static void
stop_handler_thread(void)
{
    if (!sentry__atomic_fetch(&g_handler_thread_ready)) {
        return;
    }

    sentry__atomic_store(&g_handler_should_exit, 1);
    wake_handler_thread(false);

    sentry__thread_join(g_handler_thread);
    sentry__thread_free(&g_handler_thread);
    teardown_handler_sync();

    sentry__atomic_store(&g_handler_thread_ready, 0);
    sentry__atomic_store(&g_handler_should_exit, 0);
}

static bool
has_handler_thread_crashed(void)
{
    const sentry_threadid_t current_thread = sentry__current_thread();
    if (sentry__atomic_fetch(&g_handler_thread_ready)
        && sentry__threadid_equal(current_thread, g_handler_thread)) {
#ifdef SENTRY_PLATFORM_UNIX
        SENTRY_SIGNAL_SAFE_LOG(
            "FATAL crash in handler thread, falling back to previous handler");
#else
        SENTRY_SIGNAL_SAFE_LOG(
            "FATAL crash in handler thread, UEF continues search");
#endif
        return true;
    }
    return false;
}

static bool
dispatch_ucontext(const sentry_ucontext_t *uctx,
    const struct signal_slot *sig_slot, int handler_depth)
{
    // skip_hooks when re-entering (depth >= 2) to avoid crashing in the same
    // hook again, but still try to capture the crash
    bool skip_hooks = handler_depth >= 2;

#ifdef SENTRY_WITH_UNWINDER_LIBBACKTRACE
    // For targets that still use `backtrace()` as the sole unwinder we must
    // run the signal-unsafe part in the signal handler like we did before.
    // Disable stdio-based logging - not safe in signal handler context.
    sentry__logger_disable();
    process_ucontext_deferred(uctx, sig_slot, skip_hooks);
    return true;
#else
    if (has_handler_thread_crashed()) {
        // Disable stdio-based logging since we're now in signal handler context
        // where stdio functions are not safe.
        sentry__logger_disable();

        // directly execute unsafe part in signal handler as a last chance to
        // report an error when the handler thread has crashed.
        // Always skip hooks here since the first attempt (from handler thread)
        // already failed, likely due to a crashing hook.
        process_ucontext_deferred(uctx, sig_slot, true);
        return true;
    }

    // Try to become the crash handler. Only one thread can transition
    // IDLE -> HANDLING; others will spin until DONE.
    if (!sentry__atomic_compare_swap(
            &g_crash_handling_state, CRASH_STATE_IDLE, CRASH_STATE_HANDLING)) {
        // Another thread is already handling a crash. We need to release the
        // signal handler lock before spinning, otherwise the winning thread
        // won't be able to re-enter after the handler thread ACKs.
#    ifdef SENTRY_PLATFORM_UNIX
        sentry__leave_signal_handler();
#    endif
        // Spin until they're done. Once state becomes DONE, our signal handlers
        // are unregistered, so returning from this handler will re-execute the
        // crash instruction and hit the default/previous handler.
        while (sentry__atomic_fetch(&g_crash_handling_state)
            == CRASH_STATE_HANDLING) {
            sentry__cpu_relax();
        }
        // State is now DONE: just return and let the signal propagate
        return false;
    }

    // We are the first handler. Check if handler thread is available.
    if (!sentry__atomic_fetch(&g_handler_thread_ready)) {
        // Disable stdio-based logging - not safe in signal handler context.
        sentry__logger_disable();
        process_ucontext_deferred(uctx, sig_slot, skip_hooks);
        return true;
    }

    g_handler_state.uctx = *uctx;
    g_handler_state.sig_slot = sig_slot;

#    ifdef SENTRY_PLATFORM_UNIX
    if (uctx->siginfo) {
        memcpy(&g_handler_state.siginfo_storage, uctx->siginfo,
            sizeof(g_handler_state.siginfo_storage));
        g_handler_state.uctx.siginfo = &g_handler_state.siginfo_storage;
    } else {
        g_handler_state.uctx.siginfo = NULL;
    }

    if (uctx->user_context) {
        memcpy(&g_handler_state.user_context_storage, uctx->user_context,
            sizeof(g_handler_state.user_context_storage));
        g_handler_state.uctx.user_context
            = &g_handler_state.user_context_storage;
    } else {
        g_handler_state.uctx.user_context = NULL;
    }

    // we leave the handler
    sentry__leave_signal_handler();
#    endif

    sentry__atomic_store(&g_handler_has_work, 1);

    // signal the handler thread to start working
    bool handler_signaled = false;
#    ifdef SENTRY_PLATFORM_UNIX
    if (g_handler_pipe[1] >= 0) {
        char c = 1;
        ssize_t rv;
        do {
            rv = write(g_handler_pipe[1], &c, 1);
        } while (rv == -1 && errno == EINTR);

        if (rv == 1) {
            handler_signaled = true;
        } else {
            // Write failed (EPIPE, etc.) - handler thread may be dead
            SENTRY_SIGNAL_SAFE_LOG(
                "WARN failed to signal handler thread, processing in-handler");
        }
    }

    if (!handler_signaled) {
        // Fall back to in-handler processing
        int depth = sentry__enter_signal_handler();
        if (depth >= 3) {
            // Multiple recursive crashes - bail out
            return true;
        }
        // Disable stdio-based logging - not safe in signal handler context.
        sentry__logger_disable();
        // Use skip_hooks from the original handler_depth rather than the
        // fresh depth: the lock was released above, so depth here is
        // always 1, which would incorrectly re-run hooks on a recursive
        // crash where the pipe also fails.
        process_ucontext_deferred(uctx, sig_slot, skip_hooks);
        return true;
    }
#    elif defined(SENTRY_PLATFORM_WINDOWS)
    if (g_handler_semaphore) {
        if (ReleaseSemaphore(g_handler_semaphore, 1, NULL)) {
            handler_signaled = true;
        } else {
            SENTRY_SIGNAL_SAFE_LOG(
                "WARN failed to signal handler thread, processing in-handler");
        }
    }

    if (!handler_signaled) {
        // Fall back to in-handler processing
        // Disable stdio-based logging - not safe in signal handler context.
        sentry__logger_disable();
        process_ucontext_deferred(uctx, sig_slot, skip_hooks);
        return true;
    }
#    endif

    // wait until the handler has done its work
#    ifdef SENTRY_PLATFORM_UNIX
    if (g_handler_ack_pipe[0] >= 0) {
        char c = 0;
        ssize_t rv;
        do {
            rv = read(g_handler_ack_pipe[0], &c, 1);
        } while (rv == -1 && errno == EINTR);
    }
#    elif defined(SENTRY_PLATFORM_WINDOWS)
    if (g_handler_ack_semaphore) {
        // Use a timeout rather than INFINITE to prevent hanging forever.
        // The most likely reason for this  are integration tests in CI.
        DWORD wait_result = WaitForSingleObject(g_handler_ack_semaphore, 10000);
        if (wait_result == WAIT_TIMEOUT) {
            SENTRY_SIGNAL_SAFE_LOG(
                "WARN handler thread did not ACK within timeout");
        }
    }
#    endif

#    ifdef SENTRY_PLATFORM_UNIX
    // Re-acquire signal handler lock after handler thread finished.
    (void)!sentry__enter_signal_handler();
#    endif

    return true;
#endif
}

/**
 * This is the signal-safe part of the inproc handler. Everything in here should
 * not defer to more than the set of functions listed in:
 * https://www.man7.org/linux/man-pages/man7/signal-safety.7.html
 * Since this function runs as an UnhandledExceptionFilter on Windows, the rules
 * are less strict, but similar in nature.
 *
 * That means:
 *  - no heap allocations except for sentry_malloc() (page allocator enabled!!!)
 *  - no stdio or any kind of libc string formatting
 *  - no logging (at least not with the printf-based default logger)
 *  - no thread synchronization (SENTRY_WITH_OPTIONS will terminate with a log)
 *  - in particular, don't access sentry interfaces that could request
 *    access to options or the scope, those should go to the handler thread
 *  - sentry_value_* and sentry_malloc are generally fine, because we use a safe
 *    allocator, but keep in mind that some constructors create timestampy and
 *    similar stringy and thus formatted values (and those are forbidden here).
 *
 *  If you are unsure about a particular function on a given target platform
 *  please consult the signal-safety man page.
 *
 *  Another decision marker of whether code should go in here: do you must run
 *  on the preempted crashed thread? Do you need to run before anything else?
 */
static void
process_ucontext(const sentry_ucontext_t *uctx)
{
#ifdef SENTRY_PLATFORM_LINUX
    if (g_backend_config.handler_strategy
            == SENTRY_HANDLER_STRATEGY_CHAIN_AT_START
        && uctx->signum != SIGABRT) {
        // SIGABRT is excluded: CLR/Mono never uses it for managed exception
        // translation. Chaining SIGABRT to a SIG_DFL previous handler calls
        // raise(SIGABRT), and with SA_NODEFER our handler re-enters
        // immediately causing an infinite loop.
        //
        // On Linux (and thus Android) CLR/Mono converts signals provoked by
        // AOT/JIT-generated native code into managed code exceptions. In these
        // cases, we shouldn't react to the signal at all and let their handler
        // discontinue the signal chain by invoking the runtime handler before
        // we process the signal.
        uintptr_t ip = get_instruction_pointer(uctx);
        uintptr_t sp = get_stack_pointer(uctx);

#    ifdef SENTRY_PLATFORM_ANDROID
        // Mask the signal so SA_NODEFER doesn't let re-raises from the chained
        // handler kill the process before we regain control.
        sigset_t mask, old_mask;
        sigemptyset(&mask);
        sigaddset(&mask, uctx->signum);
        // Raw syscall because ART's libsigchain intercepts
        // sigprocmask() and silently drops the request when called
        // outside its own special handlers. Without the raw syscall
        // the mask change would be ignored and SA_NODEFER would let
        // the chained handler's raise() re-deliver the signal
        // immediately, crashing the process before we can inspect
        // the modified IP/SP.
        //
        // DANGER: this makes libsigchain's internal mask state
        // diverge from the kernel's actual mask. If ART ever relies
        // on that state for correctness (e.g. GC safepoints), this
        // could cause subtle failures. We restore the mask right
        // after the chained handler returns, limiting the window.
        syscall(
            SYS_rt_sigprocmask, SIG_BLOCK, &mask, &old_mask, sizeof(sigset_t));
#    endif

        // invoke the previous handler (typically the CLR/Mono
        // signal-to-managed-exception handler)
        invoke_signal_handler(
            uctx->signum, uctx->siginfo, (void *)uctx->user_context);

        // If the execution returns here in AOT mode, and the instruction
        // or stack pointer were changed, it means CLR/Mono converted the
        // signal into a managed exception and transferred execution to a
        // managed exception handler.
        // https://github.com/dotnet/runtime/blob/6d96e28597e7da0d790d495ba834cc4908e442cd/src/mono/mono/mini/exceptions-arm64.c#L538
        if (ip != get_instruction_pointer(uctx)
            || sp != get_stack_pointer(uctx)) {
            return;
        }

#    ifdef SENTRY_PLATFORM_ANDROID
        // restore our handler after resend_signal() set SIG_DFL
        sigaction(uctx->signum, &g_sigaction, NULL);

        // consume pending signal
        struct timespec timeout = { 0, 0 };
        syscall(SYS_rt_sigtimedwait, &mask, NULL, &timeout, sizeof(sigset_t));

        // unmask
        syscall(
            SYS_rt_sigprocmask, SIG_SETMASK, &old_mask, NULL, sizeof(sigset_t));
#    endif

        // return from runtime handler; continue processing the crash on the
        // signal thread until the worker takes over
    }
#endif

#ifdef SENTRY_PLATFORM_UNIX
    int handler_depth = sentry__enter_signal_handler();
    if (handler_depth >= 3) {
        // Multiple recursive crashes - bail out completely to avoid loops
        SENTRY_SIGNAL_SAFE_LOG(
            "multiple recursive crashes detected, bailing out");
        goto cleanup;
    }

    // If we were only preloaded (before sentry_init()) or were closed after
    // preload (handlers still installed, but no handler thread), do not attempt
    // full crash capture here. In this state, preload only serves as a
    // placeholder in the signal chain.
    //
    // We therefore remove our placeholder from the chain and forward the
    // signal to the previously installed handler set.
    //
    // This path is expected to be terminal for real crash signals. If it
    // returns, preload mode is consumed for the remainder of the process
    // lifetime until a later sentry_init() reinstalls handlers from the
    // then-current chain.
    //
    // This also covers the window after preload but before the managed runtime
    // has installed its own handlers: a signal seen in that window is forwarded
    // to the pre-preload handler set rather than being captured by Sentry.
    if (sentry__atomic_fetch(&g_preloaded)
        && !sentry__atomic_fetch(&g_handler_thread_ready)) {
        SENTRY_SIGNAL_SAFE_LOG(
            "handler thread not ready, falling through to previous handler");
        reset_signal_handlers();
        sentry__atomic_store(&g_preloaded, 0);
        sentry__leave_signal_handler();
        invoke_signal_handler(
            uctx->signum, uctx->siginfo, (void *)uctx->user_context);
        return;
    }
#endif

    if (!g_backend_config.enable_logging_when_crashed) {
        sentry__logger_disable();
    }

    const struct signal_slot *sig_slot = NULL;
    for (int i = 0; i < SIGNAL_COUNT; ++i) {
#ifdef SENTRY_PLATFORM_UNIX
        if (SIGNAL_DEFINITIONS[i].signum == uctx->signum) {
#elif defined SENTRY_PLATFORM_WINDOWS
        if (SIGNAL_DEFINITIONS[i].signum
            == uctx->exception_ptrs.ExceptionRecord->ExceptionCode) {
#else
#    error Unsupported platform
#endif
            sig_slot = &SIGNAL_DEFINITIONS[i];
        }
    }

#ifdef SENTRY_PLATFORM_UNIX
    // use a signal-safe allocator before we tear down.
    sentry__page_allocator_enable();
#endif

    // invoke the handler thread for signal unsafe actions
#ifdef SENTRY_PLATFORM_UNIX
    bool is_handling_thread = dispatch_ucontext(uctx, sig_slot, handler_depth);
#else
    bool is_handling_thread = dispatch_ucontext(uctx, sig_slot, 1);
#endif

    // Non-handling threads (that lost the CAS race) should just return.
    // Signal handlers are already reset by the handling thread, so
    // re-executing the crashing instruction will hit the default handler.
    if (!is_handling_thread) {
        return;
    }

#ifdef SENTRY_PLATFORM_UNIX
cleanup:
    // reset signal handlers and invoke the original ones.  This will then tear
    // down the process.  In theory someone might have some other handler here
    // which recovers the process but this will cause a memory leak going
    // forward as we're not restoring the page allocator.
    reset_signal_handlers();

    // Signal to any other threads spinning in dispatch_ucontext that we're
    // done. They can now return from their signal handlers. Since our handlers
    // are unregistered, re-executing their crash will hit the default handler.
    sentry__atomic_store(&g_crash_handling_state, CRASH_STATE_DONE);

    sentry__leave_signal_handler();
    if (g_backend_config.handler_strategy
        != SENTRY_HANDLER_STRATEGY_CHAIN_AT_START) {
        invoke_signal_handler(
            uctx->signum, uctx->siginfo, (void *)uctx->user_context);
    }
#elif defined(SENTRY_PLATFORM_WINDOWS)
    // Signal to any other threads spinning in dispatch_ucontext that we're
    // done. They can now return EXCEPTION_CONTINUE_SEARCH from their UEF,
    // allowing the exception to propagate and terminate the process.
    sentry__atomic_store(&g_crash_handling_state, CRASH_STATE_DONE);
#endif
}

#ifdef SENTRY_PLATFORM_UNIX
static void
handle_signal(int signum, siginfo_t *info, void *user_context)
{
    sentry_ucontext_t uctx;
    uctx.signum = signum;
    uctx.siginfo = info;
    uctx.user_context = (ucontext_t *)user_context;
    process_ucontext(&uctx);
}
#elif defined SENTRY_PLATFORM_WINDOWS
static LONG WINAPI
handle_exception(EXCEPTION_POINTERS *ExceptionInfo)
{
    if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT
        || ExceptionInfo->ExceptionRecord->ExceptionCode
            == EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    sentry_ucontext_t uctx = { 0 };
    uctx.exception_ptrs = *ExceptionInfo;
    process_ucontext(&uctx);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static void
handle_except(sentry_backend_t *UNUSED(backend), const sentry_ucontext_t *uctx)
{
    process_ucontext(uctx);
}

void
sentry__backend_preload(void)
{
#ifdef SENTRY_PLATFORM_UNIX
    if (install_signal_handlers() == 0) {
        sentry__atomic_store(&g_preloaded, 1);
    }
#endif
}

sentry_backend_t *
sentry__backend_new(void)
{
    sentry_backend_t *backend = SENTRY_MAKE(sentry_backend_t);
    if (!backend) {
        return NULL;
    }

    backend->startup_func = startup_inproc_backend;
    backend->shutdown_func = shutdown_inproc_backend;
    backend->except_func = handle_except;

    return backend;
}
