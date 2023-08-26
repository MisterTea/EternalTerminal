#include "sentry_boot.h"

#include "sentry_alloc.h"
#include "sentry_backend.h"
#include "sentry_core.h"
#include "sentry_database.h"
#include "sentry_envelope.h"
#include "sentry_options.h"
#include "sentry_scope.h"
#include "sentry_sync.h"
#include "sentry_transport.h"
#include "sentry_unix_pageallocator.h"
#include "transports/sentry_disk_transport.h"
#include <string.h>

#define SIGNAL_DEF(Sig, Desc)                                                  \
    {                                                                          \
        Sig, #Sig, Desc                                                        \
    }

#define MAX_FRAMES 128

#ifdef SENTRY_PLATFORM_UNIX
struct signal_slot {
    int signum;
    const char *signame;
    const char *sigdesc;
};

// we need quite a bit of space for backtrace generation
#    define SIGNAL_COUNT 6
#    define SIGNAL_STACK_SIZE 65536
static struct sigaction g_sigaction;
static struct sigaction g_previous_handlers[SIGNAL_COUNT];
static stack_t g_signal_stack;

static const struct signal_slot SIGNAL_DEFINITIONS[SIGNAL_COUNT] = {
    SIGNAL_DEF(SIGILL, "IllegalInstruction"),
    SIGNAL_DEF(SIGTRAP, "Trap"),
    SIGNAL_DEF(SIGABRT, "Abort"),
    SIGNAL_DEF(SIGBUS, "BusError"),
    SIGNAL_DEF(SIGFPE, "FloatingPointException"),
    SIGNAL_DEF(SIGSEGV, "Segfault"),
};

static void handle_signal(int signum, siginfo_t *info, void *user_context);

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
startup_inproc_backend(
    sentry_backend_t *UNUSED(backend), const sentry_options_t *UNUSED(options))
{
    // save the old signal handlers
    memset(g_previous_handlers, 0, sizeof(g_previous_handlers));
    for (size_t i = 0; i < SIGNAL_COUNT; ++i) {
        if (sigaction(
                SIGNAL_DEFINITIONS[i].signum, NULL, &g_previous_handlers[i])
            == -1) {
            return 1;
        }
    }

    // install our own signal handler
    g_signal_stack.ss_sp = sentry_malloc(SIGNAL_STACK_SIZE);
    if (!g_signal_stack.ss_sp) {
        return 1;
    }
    g_signal_stack.ss_size = SIGNAL_STACK_SIZE;
    g_signal_stack.ss_flags = 0;
    sigaltstack(&g_signal_stack, 0);

    sigemptyset(&g_sigaction.sa_mask);
    g_sigaction.sa_sigaction = handle_signal;
    g_sigaction.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (size_t i = 0; i < SIGNAL_COUNT; ++i) {
        sigaction(SIGNAL_DEFINITIONS[i].signum, &g_sigaction, NULL);
    }
    return 0;
}

static void
shutdown_inproc_backend(sentry_backend_t *UNUSED(backend))
{
    g_signal_stack.ss_flags = SS_DISABLE;
    sigaltstack(&g_signal_stack, 0);
    sentry_free(g_signal_stack.ss_sp);
    g_signal_stack.ss_sp = NULL;
    reset_signal_handlers();
}

#elif defined(SENTRY_PLATFORM_WINDOWS)

struct signal_slot {
    DWORD signum;
    const char *signame;
    const char *sigdesc;
};

#    define SIGNAL_COUNT 20

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
    SIGNAL_DEF(EXCEPTION_STACK_OVERFLOW, "StackOverflow")
};

static LONG WINAPI handle_exception(EXCEPTION_POINTERS *);

static int
startup_inproc_backend(
    sentry_backend_t *UNUSED(backend), const sentry_options_t *UNUSED(options))
{
    g_previous_handler = SetUnhandledExceptionFilter(&handle_exception);
    SetErrorMode(SEM_FAILCRITICALERRORS);
    return 0;
}

static void
shutdown_inproc_backend(sentry_backend_t *UNUSED(backend))
{
    LPTOP_LEVEL_EXCEPTION_FILTER current_handler
        = SetUnhandledExceptionFilter(g_previous_handler);
    if (current_handler != &handle_exception) {
        SetUnhandledExceptionFilter(current_handler);
    }
}

#endif

sentry_value_t
sentry__registers_from_uctx(const sentry_ucontext_t *uctx)
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
    SET_REG("fp", __fp);
    SET_REG("lr", __lr);
    SET_REG("sp", __sp);
    SET_REG("pc", __pc);

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
            sentry__value_new_addr((uint64_t)(size_t)ctx->prop));

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

#    else
    // _ARM64_
#    endif

#    undef SET_REG

#endif

    return registers;
}

static sentry_value_t
make_signal_event(
    const struct signal_slot *sig_slot, const sentry_ucontext_t *uctx)
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
        // at least on windows, the signum is a true u32 which we can't
        // otherwise represent.
        sentry_value_set_by_key(signal_meta, "number",
            sentry_value_new_double((double)sig_slot->signum));
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
    // if unwinding from a ucontext didn't yield any results, try again with a
    // direct unwind. this is most likely the case when using `libbacktrace`,
    // since that does not allow to unwind from a ucontext at all.
    if (!frame_count) {
        frame_count = sentry_unwind_stack(NULL, &backtrace[0], MAX_FRAMES);
    }
    SENTRY_TRACEF("captured backtrace with %lu frames", frame_count);

    sentry_value_t stacktrace
        = sentry_value_new_stacktrace(&backtrace[0], frame_count);

    sentry_value_t registers = sentry__registers_from_uctx(uctx);
    sentry_value_set_by_key(stacktrace, "registers", registers);

    sentry_value_set_by_key(exc, "stacktrace", stacktrace);
    sentry_event_add_exception(event, exc);

    return event;
}

static void
handle_ucontext(const sentry_ucontext_t *uctx)
{
    SENTRY_DEBUG("entering signal handler");

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
    // give us an allocator we can use safely in signals before we tear down.
    sentry__page_allocator_enable();

    // inform the sentry_sync system that we're in a signal handler.  This will
    // make mutexes spin on a spinlock instead as it's no longer safe to use a
    // pthread mutex.
    sentry__enter_signal_handler();
#endif

    sentry_value_t event = make_signal_event(sig_slot, uctx);

    SENTRY_WITH_OPTIONS (options) {
        sentry__write_crash_marker(options);

        bool should_handle = true;

        if (options->on_crash_func) {
            SENTRY_TRACE("invoking `on_crash` hook");
            event = options->on_crash_func(uctx, event, options->on_crash_data);
            should_handle = !sentry_value_is_null(event);
        }

        if (should_handle) {
            sentry_envelope_t *envelope = sentry__prepare_event(
                options, event, NULL, !options->on_crash_func);
            // TODO(tracing): Revisit when investigating transaction flushing
            // during hard crashes.

            sentry_session_t *session = sentry__end_current_session_with_status(
                SENTRY_SESSION_STATUS_CRASHED);
            sentry__envelope_add_session(envelope, session);

            // capture the envelope with the disk transport
            sentry_transport_t *disk_transport
                = sentry_new_disk_transport(options->run);
            sentry__capture_envelope(disk_transport, envelope);
            sentry__transport_dump_queue(disk_transport, options->run);
            sentry_transport_free(disk_transport);
        } else {
            SENTRY_TRACE("event was discarded by the `on_crash` hook");
            sentry_value_decref(event);
        }

        // after capturing the crash event, dump all the envelopes to disk
        sentry__transport_dump_queue(options->transport, options->run);
    }

    SENTRY_DEBUG("crash has been captured");

#ifdef SENTRY_PLATFORM_UNIX
    // reset signal handlers and invoke the original ones.  This will then tear
    // down the process.  In theory someone might have some other handler here
    // which recovers the process but this will cause a memory leak going
    // forward as we're not restoring the page allocator.
    reset_signal_handlers();
    sentry__leave_signal_handler();
    invoke_signal_handler(
        uctx->signum, uctx->siginfo, (void *)uctx->user_context);
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
    handle_ucontext(&uctx);
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

    sentry_ucontext_t uctx;
    memset(&uctx, 0, sizeof(uctx));
    uctx.exception_ptrs = *ExceptionInfo;
    handle_ucontext(&uctx);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static void
handle_except(sentry_backend_t *UNUSED(backend), const sentry_ucontext_t *uctx)
{
    handle_ucontext(uctx);
}

sentry_backend_t *
sentry__backend_new(void)
{
    sentry_backend_t *backend = SENTRY_MAKE(sentry_backend_t);
    if (!backend) {
        return NULL;
    }
    memset(backend, 0, sizeof(sentry_backend_t));

    backend->startup_func = startup_inproc_backend;
    backend->shutdown_func = shutdown_inproc_backend;
    backend->except_func = handle_except;

    return backend;
}
