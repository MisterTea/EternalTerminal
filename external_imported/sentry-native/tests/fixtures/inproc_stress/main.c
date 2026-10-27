/**
 * Main test driver for inproc backend stress tests.
 *
 * Usage:
 *   ./inproc_stress_test concurrent-crash
 *   ./inproc_stress_test handler-thread-crash
 *   ./inproc_stress_test simple-crash
 *   ./inproc_stress_test pipe-failure
 */

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#endif

#include "sentry.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#    include <signal.h>
#    include <unistd.h>
#endif

// From concurrent_crash.c
extern void run_concurrent_crash(void);

static void *invalid_mem = (void *)1;

// Prevent inlining and optimization to ensure predictable frame records.
#if defined(__clang__)
#    define NOINLINE __attribute__((noinline, optnone))
#elif defined(__GNUC__)
#    define NOINLINE __attribute__((noinline, optimize("O0")))
#else
#    define NOINLINE __attribute__((noinline))
#endif

// Stack trace test functions (used on macOS because that uses a handwritten stack-walker, can compile on Linux/Unix).
// The naming convention encodes the expected call chain so the Python test
// can verify frames by function name.
//
// Scenario 1: crash in function with frame record, no sub-calls since prologue
//   stacktest_A_calls_B_no_subcalls -> stacktest_B_crash_no_subcalls
//
// Scenario 2: crash in function with frame record, made sub-calls before crash
//   stacktest_A_calls_B_with_subcalls -> stacktest_B_crash_after_subcall
//     (B calls stacktest_C_helper which returns, then B crashes)
//
// Scenario 3: crash in function without a frame record
//   stacktest_A_calls_B_no_frame_record -> stacktest_B_crash_no_frame_record
#ifdef __APPLE__

static volatile int g_side_effect = 0;

// Has a frame record (explicit prologue) but makes no calls, so LR still
// holds the return address from the caller's bl, matching [FP+8].
#    if defined(__aarch64__)
__attribute__((naked, noinline)) static void
stacktest_B_crash_no_subcalls(void)
{
    __asm__ volatile(
        "stp x29, x30, [sp, #-16]!\n\t" // prologue: save fp, lr
        "mov x29, sp\n\t"               // set up frame pointer
        "mov x8, #1\n\t"
        "str wzr, [x8]\n\t"             // store to address 0x1 -> SIGSEGV
    );
}
#    elif defined(__x86_64__)
__attribute__((naked, noinline)) static void
stacktest_B_crash_no_subcalls(void)
{
    __asm__ volatile(
        "pushq %%rbp\n\t"               // prologue: save bp
        "movq %%rsp, %%rbp\n\t"         // set up frame pointer
        "movl $0, 0x1\n\t"              // store to address 0x1 -> SIGSEGV
        ::: "memory"
    );
}
#    endif

NOINLINE static void
stacktest_A_calls_B_no_subcalls(void)
{
    fprintf(stderr, "stacktest_A_calls_B_no_subcalls\n");
    fflush(stderr);
    stacktest_B_crash_no_subcalls();
}

NOINLINE static void
stacktest_C_helper(void)
{
    // A function that returns normally, just to modify LR in the caller
    g_side_effect = 42;
}

NOINLINE static void
stacktest_B_crash_after_subcall(void)
{
    fprintf(stderr, "stacktest_B_crash_after_subcall\n");
    fflush(stderr);
    // Call a helper (modifies LR), then crash without another call
    stacktest_C_helper();
    *(volatile int *)invalid_mem = 0xdead;
}

NOINLINE static void
stacktest_A_calls_B_with_subcalls(void)
{
    fprintf(stderr, "stacktest_A_calls_B_with_subcalls\n");
    fflush(stderr);
    stacktest_B_crash_after_subcall();
}

// This function must not have a frame record. We use __attribute__((naked))
// to suppress the prologue/epilogue entirely, and write the crashing store
// in assembly. This ensures FP still points to the caller's frame.
#    if defined(__aarch64__)
__attribute__((naked, noinline)) static void
stacktest_B_crash_no_frame_record(void)
{
    __asm__ volatile(
        "mov x8, #1\n\t"
        "str wzr, [x8]\n\t" // store to address 0x1 -> SIGSEGV
    );
}
#    elif defined(__x86_64__)
__attribute__((naked, noinline)) static void
stacktest_B_crash_no_frame_record(void)
{
    __asm__ volatile(
        "movl $0, 0x1\n\t" // store to address 0x1 -> SIGSEGV
    );
}
#    endif

NOINLINE static void
stacktest_A_calls_B_no_frame_record(void)
{
    fprintf(stderr, "stacktest_A_calls_B_no_frame_record\n");
    fflush(stderr);
    stacktest_B_crash_no_frame_record();
}

#endif /* __APPLE__ */

// on_crash callback that crashes via SIGSEGV: simulates buggy user code
static sentry_value_t
crashing_on_crash_callback(
    const sentry_ucontext_t *uctx, sentry_value_t event, void *closure)
{
    (void)uctx;
    (void)event;
    (void)closure;

    fprintf(stderr, "on_crash callback about to crash\n");
    fflush(stderr);

    memset((char *)invalid_mem, 1, 100);

    return event;
}

// on_crash callback that crashes via abort(): tests signal mask reset behavior
static sentry_value_t
aborting_on_crash_callback(
    const sentry_ucontext_t *uctx, sentry_value_t event, void *closure)
{
    (void)uctx;
    (void)event;
    (void)closure;

    fprintf(stderr, "on_crash callback about to abort\n");
    fflush(stderr);

    abort();

    return event;
}

static void
print_envelope(sentry_envelope_t *envelope, void *unused_state)
{
    (void)unused_state;
    size_t size_out = 0;
    char *s = sentry_envelope_serialize(envelope, &size_out);
    printf("%s", s);
    fflush(stdout);
    sentry_free(s);
    sentry_envelope_free(envelope);
}

// Use wide-char paths on Windows, narrow paths elsewhere
#if defined(_WIN32) && defined(_MSC_VER)
#    define PATH_TYPE const wchar_t *
#    define SET_DATABASE_PATH sentry_options_set_database_pathw
#    define DEFAULT_DATABASE_PATH L".sentry-native"
#else
#    define PATH_TYPE const char *
#    define SET_DATABASE_PATH sentry_options_set_database_path
#    define DEFAULT_DATABASE_PATH ".sentry-native"
#endif

static int
setup_sentry(PATH_TYPE database_path)
{
    sentry_options_t *options = sentry_options_new();
    SET_DATABASE_PATH(options, database_path);
    sentry_options_set_auto_session_tracking(options, false);
    sentry_options_set_dsn(options, "https://public@sentry.invalid/1");
    sentry_options_set_debug(options, 1);
    sentry_options_set_symbolize_stacktraces(options, 1);
    sentry_options_set_transport(options, sentry_transport_new(print_envelope));

    if (sentry_init(options) != 0) {
        fprintf(stderr, "Failed to initialize sentry\n");
        return 1;
    }
    return 0;
}

static int
test_concurrent_crash(PATH_TYPE database_path)
{
    if (setup_sentry(database_path) != 0) {
        return 1;
    }

    sentry_set_tag("test", "concurrent-crash");

    fprintf(stderr, "Starting concurrent crash test\n");
    fflush(stderr);

    run_concurrent_crash();

    // Should not reach this
    fprintf(stderr, "ERROR: Threads returned without crashing\n");
    sentry_close();
    return 1;
}

static void
trigger_crash(void)
{
    memset((char *)invalid_mem, 1, 100);
}

static int
setup_sentry_with_crashing_on_crash(PATH_TYPE database_path)
{
    sentry_options_t *options = sentry_options_new();
    SET_DATABASE_PATH(options, database_path);
    sentry_options_set_auto_session_tracking(options, false);
    sentry_options_set_dsn(options, "https://public@sentry.invalid/1");
    sentry_options_set_debug(options, 1);
    sentry_options_set_transport(options, sentry_transport_new(print_envelope));

    // Set the crashing on_crash callback
    sentry_options_set_on_crash(options, crashing_on_crash_callback, NULL);

    if (sentry_init(options) != 0) {
        fprintf(stderr, "Failed to initialize sentry\n");
        return 1;
    }
    return 0;
}

static int
setup_sentry_with_aborting_on_crash(PATH_TYPE database_path)
{
    sentry_options_t *options = sentry_options_new();
    SET_DATABASE_PATH(options, database_path);
    sentry_options_set_auto_session_tracking(options, false);
    sentry_options_set_dsn(options, "https://public@sentry.invalid/1");
    sentry_options_set_debug(options, 1);
    sentry_options_set_transport(options, sentry_transport_new(print_envelope));

    sentry_options_set_on_crash(options, aborting_on_crash_callback, NULL);

    if (sentry_init(options) != 0) {
        fprintf(stderr, "Failed to initialize sentry\n");
        return 1;
    }
    return 0;
}

static int
test_handler_thread_crash(PATH_TYPE database_path)
{
    if (setup_sentry_with_crashing_on_crash(database_path) != 0) {
        return 1;
    }

    sentry_set_tag("test", "handler-thread-crash");

    fprintf(stderr, "Starting handler thread crash test\n");
    fflush(stderr);

    // This will crash, trigger the handler thread, which will call
    // on_crash callback, which will crash the handler thread.
    // The fallback should then process in the signal handler.
    trigger_crash();

    fprintf(stderr, "ERROR: Should have crashed\n");
    sentry_close();
    return 1;
}

static int
test_handler_abort_crash(PATH_TYPE database_path)
{
    if (setup_sentry_with_aborting_on_crash(database_path) != 0) {
        return 1;
    }

    sentry_set_tag("test", "handler-abort-crash");

    fprintf(stderr, "Starting handler abort crash test\n");
    fflush(stderr);

    // This will crash, trigger the handler thread, which will call
    // on_crash callback, which will call abort(). abort() resets the
    // signal mask, so this tests a different code path.
    trigger_crash();

    fprintf(stderr, "ERROR: Should have crashed\n");
    sentry_close();
    return 1;
}

#ifdef __APPLE__
static int
test_stack_no_subcalls(PATH_TYPE database_path)
{
    if (setup_sentry(database_path) != 0) {
        return 1;
    }
    sentry_set_tag("test", "stack-no-subcalls");
    fprintf(stderr, "Starting stack-no-subcalls test\n");
    fflush(stderr);
    stacktest_A_calls_B_no_subcalls();
    fprintf(stderr, "ERROR: Should have crashed\n");
    sentry_close();
    return 1;
}

static int
test_stack_with_subcalls(PATH_TYPE database_path)
{
    if (setup_sentry(database_path) != 0) {
        return 1;
    }
    sentry_set_tag("test", "stack-with-subcalls");
    fprintf(stderr, "Starting stack-with-subcalls test\n");
    fflush(stderr);
    stacktest_A_calls_B_with_subcalls();
    fprintf(stderr, "ERROR: Should have crashed\n");
    sentry_close();
    return 1;
}

static int
test_stack_no_frame_record(PATH_TYPE database_path)
{
    if (setup_sentry(database_path) != 0) {
        return 1;
    }
    sentry_set_tag("test", "stack-no-frame-record");
    fprintf(stderr, "Starting stack-no-frame-record test\n");
    fflush(stderr);
    stacktest_A_calls_B_no_frame_record();
    fprintf(stderr, "ERROR: Should have crashed\n");
    sentry_close();
    return 1;
}
#endif /* __APPLE__ */

static int
test_simple_crash(PATH_TYPE database_path)
{
    if (setup_sentry(database_path) != 0) {
        return 1;
    }

    sentry_set_tag("test", "simple-crash");

    fprintf(stderr, "Starting simple crash test\n");
    fflush(stderr);

    trigger_crash();

    fprintf(stderr, "ERROR: Should have crashed\n");
    sentry_close();
    return 1;
}

#if defined(_WIN32) && defined(_MSC_VER)
int
wmain(int argc, wchar_t *argv[])
{
    // Suppress the abort() dialog so CI doesn't block.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    if (argc < 2) {
        fwprintf(stderr, L"Usage: %ls <test-name> [database-path]\n", argv[0]);
        fwprintf(stderr, L"Tests:\n");
        fwprintf(stderr,
            L"  concurrent-crash       - Multiple threads crash "
            L"simultaneously\n");
        fwprintf(stderr,
            L"  simple-crash           - Single thread crash (baseline)\n");
        fwprintf(stderr,
            L"  handler-thread-crash   - Handler thread crashes in on_crash "
            L"(SIGSEGV)\n");
        fwprintf(stderr,
            L"  handler-abort-crash    - Handler thread crashes in on_crash "
            L"(abort)\n");
        return 1;
    }

    const wchar_t *test_name = argv[1];
    const wchar_t *database_path
        = argc > 2 ? argv[2] : DEFAULT_DATABASE_PATH;

    if (wcscmp(test_name, L"concurrent-crash") == 0) {
        return test_concurrent_crash(database_path);
    }
    if (wcscmp(test_name, L"simple-crash") == 0) {
        return test_simple_crash(database_path);
    }
    if (wcscmp(test_name, L"handler-thread-crash") == 0) {
        return test_handler_thread_crash(database_path);
    }
    if (wcscmp(test_name, L"handler-abort-crash") == 0) {
        return test_handler_abort_crash(database_path);
    }
    fwprintf(stderr, L"Unknown test: %ls\n", test_name);
    return 1;
}
#else
int
main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <test-name> [database-path]\n", argv[0]);
        fprintf(stderr, "Tests:\n");
        fprintf(stderr,
            "  concurrent-crash       - Multiple threads crash "
            "simultaneously\n");
        fprintf(stderr,
            "  simple-crash           - Single thread crash (baseline)\n");
        fprintf(stderr,
            "  handler-thread-crash   - Handler thread crashes in on_crash "
            "(SIGSEGV)\n");
        fprintf(stderr,
            "  handler-abort-crash    - Handler thread crashes in on_crash "
            "(abort)\n");
#ifdef __APPLE__
        fprintf(stderr,
            "  stack-no-subcalls      - Stack trace: no sub-calls\n");
        fprintf(stderr,
            "  stack-with-subcalls    - Stack trace: with sub-calls\n");
        fprintf(stderr,
            "  stack-no-frame-record  - Stack trace: no frame record\n");
#endif
        return 1;
    }

    const char *test_name = argv[1];
    const char *database_path = argc > 2 ? argv[2] : DEFAULT_DATABASE_PATH;

    if (strcmp(test_name, "concurrent-crash") == 0) {
        return test_concurrent_crash(database_path);
    }
    if (strcmp(test_name, "simple-crash") == 0) {
        return test_simple_crash(database_path);
    }
    if (strcmp(test_name, "handler-thread-crash") == 0) {
        return test_handler_thread_crash(database_path);
    }
    if (strcmp(test_name, "handler-abort-crash") == 0) {
        return test_handler_abort_crash(database_path);
    }
#ifdef __APPLE__
    if (strcmp(test_name, "stack-no-subcalls") == 0) {
        return test_stack_no_subcalls(database_path);
    }
    if (strcmp(test_name, "stack-with-subcalls") == 0) {
        return test_stack_with_subcalls(database_path);
    }
    if (strcmp(test_name, "stack-no-frame-record") == 0) {
        return test_stack_no_frame_record(database_path);
    }
#endif
    fprintf(stderr, "Unknown test: %s\n", test_name);
    return 1;
}
#endif
