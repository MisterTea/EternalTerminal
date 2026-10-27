#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    define NOMINMAX
#    define _CRT_SECURE_NO_WARNINGS
#endif

#include "sentry.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef NDEBUG
#    undef NDEBUG
#endif

#include <assert.h>

#ifdef SENTRY_PLATFORM_WINDOWS
#    include <malloc.h>
#    include <process.h>
#    include <synchapi.h>
#    define sleep_s(SECONDS) Sleep((SECONDS) * 1000)
#else

#    include <pthread.h>
#    include <signal.h>
#    include <unistd.h>

#    define sleep_s(SECONDS) sleep(SECONDS)
#endif

#if defined(SENTRY_PLATFORM_WINDOWS)
#    include <windows.h>
unsigned long
get_current_thread_id()
{
    return GetCurrentThreadId();
}
#elif defined(SENTRY_PLATFORM_MACOS)
#    include <pthread.h>
#    include <stdint.h>
uint64_t
get_current_thread_id()
{
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    return tid;
}
#else
#    include <stdint.h>
#    include <sys/syscall.h>
#    include <unistd.h>
uint64_t
get_current_thread_id()
{
    return (uint64_t)syscall(SYS_gettid);
}
#endif

static double
traces_sampler_callback(const sentry_transaction_context_t *transaction_ctx,
    sentry_value_t custom_sampling_ctx, const int *parent_sampled,
    void *user_data)
{
    (void)user_data;

    if (parent_sampled != NULL) {
        if (*parent_sampled) {
            return 0.8; // high sample rate for children of sampled transactions
        }
        return 0; // parent is not sampled
    }
    if (strcmp(sentry_transaction_context_get_name(transaction_ctx),
            "little.teapot")
        == 0) {
        if (strcmp(sentry_transaction_context_get_operation(transaction_ctx),
                "Short and stout here is my handle and here is my spout")
            == 0) {
            if (sentry_value_as_int32(
                    sentry_value_get_by_key(custom_sampling_ctx, "b"))
                == 42) {
                return 1;
            }
        }
    }
    return 0;
}

static sentry_value_t
before_send_callback(sentry_value_t event, void *hint, void *user_data)
{
    (void)hint;
    (void)user_data;

    // make our mark on the event
    sentry_value_set_by_key(
        event, "adapted_by", sentry_value_new_string("before_send"));

    // tell the backend to proceed with the event
    return event;
}

static sentry_value_t
discarding_before_send_callback(
    sentry_value_t event, void *hint, void *user_data)
{
    (void)hint;
    (void)user_data;

    // discard event and signal backend to stop further processing
    sentry_value_decref(event);
    return sentry_value_new_null();
}

static sentry_value_t
discarding_on_crash_callback(
    const sentry_ucontext_t *uctx, sentry_value_t event, void *user_data)
{
    (void)uctx;
    (void)user_data;

    // discard event and signal backend to stop further processing
    sentry_value_decref(event);
    return sentry_value_new_null();
}

static sentry_value_t
on_crash_callback(
    const sentry_ucontext_t *uctx, sentry_value_t event, void *user_data)
{
    (void)uctx;
    (void)user_data;

    // tell the backend to retain the event
    return event;
}

static sentry_value_t
restart_on_crash(
    const sentry_ucontext_t *uctx, sentry_value_t event, void *user_data)
{
    (void)uctx;

#ifdef SENTRY_PLATFORM_WINDOWS
    wchar_t **argv = user_data;
    if (argv && argv[0]) {
        _wspawnv(_P_NOWAIT, argv[0], (const wchar_t *const *)argv);
    }
#else
    char **argv = user_data;
    if (!argv || !argv[0]) {
        return event;
    }
    if (fork() == 0) {
        // The crashing signal is blocked while the crash handler runs. Do not
        // let the restarted child inherit that mask.
        sigset_t set;
        sigfillset(&set);
        sigprocmask(SIG_UNBLOCK, &set, NULL);

        execv(argv[0], argv);
        _exit(127);
    }
#endif

    return event;
}

// Forward all original arguments except "restart-on-crash"
static void *
restart_args(int argc, char **argv)
{
#ifdef SENTRY_PLATFORM_WINDOWS
    wchar_t **child_argv = calloc((size_t)argc + 1, sizeof(wchar_t *));
    if (!child_argv) {
        return NULL;
    }

    child_argv[0] = calloc(MAX_PATH, sizeof(wchar_t));
    if (!child_argv[0]
        || GetModuleFileNameW(NULL, child_argv[0], MAX_PATH) == 0) {
        return child_argv;
    }

    int child_argc = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "restart-on-crash") == 0) {
            continue;
        }

        size_t len = strlen(argv[i]) + 1;
        child_argv[child_argc] = calloc(len, sizeof(wchar_t));
        if (!child_argv[child_argc]) {
            return child_argv;
        }
        mbstowcs(child_argv[child_argc++], argv[i], len);
    }
    return child_argv;
#else
    char **child_argv = calloc((size_t)argc + 1, sizeof(char *));
    if (!child_argv) {
        return NULL;
    }

    int child_argc = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "restart-on-crash") != 0) {
            child_argv[child_argc++] = argv[i];
        }
    }
    return child_argv;
#endif
}

static sentry_value_t
before_transaction_callback(sentry_value_t tx, void *user_data)
{
    (void)user_data;

    sentry_value_set_by_key(
        tx, "transaction", sentry_value_new_string("little.coffeepot"));
    return tx;
}

static sentry_value_t
discarding_before_transaction_callback(sentry_value_t tx, void *user_data)
{
    (void)user_data;
    // throw out any transaction while a tag is active
    if (!sentry_value_is_null(sentry_value_get_by_key(tx, "tags"))) {
        sentry_value_decref(tx);
        return sentry_value_new_null();
    }
    return tx;
}

static sentry_value_t
before_send_log_callback(sentry_value_t log, void *user_data)
{
    (void)user_data;
    sentry_value_t attribute
        = sentry_value_new_attribute(sentry_value_new_string("little"), NULL);
    sentry_value_set_by_key(sentry_value_get_by_key(log, "attributes"),
        "coffeepot.size", attribute);
    return log;
}

static sentry_value_t
discarding_before_send_log_callback(sentry_value_t log, void *user_data)
{
    (void)user_data;
    if (sentry_value_is_null(
            sentry_value_get_by_key(sentry_value_get_by_key(log, "attributes"),
                "sentry.message.template"))) {
        sentry_value_decref(log);
        return sentry_value_new_null();
    }
    return log;
}

static sentry_value_t
before_send_metric_callback(sentry_value_t metric, void *user_data)
{
    (void)user_data;
    sentry_value_t attribute
        = sentry_value_new_attribute(sentry_value_new_string("little"), NULL);
    sentry_value_set_by_key(sentry_value_get_by_key(metric, "attributes"),
        "coffeepot.size", attribute);
    return metric;
}

static sentry_value_t
discarding_before_send_metric_callback(sentry_value_t metric, void *user_data)
{
    (void)user_data;
    sentry_value_decref(metric);
    return sentry_value_new_null();
}

static sentry_value_t
before_breadcrumb_callback(sentry_value_t breadcrumb, void *user_data)
{
    (void)user_data;

    // make our mark on the breadcrumbs
    sentry_value_set_by_key(
        breadcrumb, "category", sentry_value_new_string("before_breadcrumb"));

    return breadcrumb;
}

static sentry_value_t
discarding_before_breadcrumb_callback(
    sentry_value_t breadcrumb, void *user_data)
{
    (void)user_data;

    // discard breadcrumb
    sentry_value_decref(breadcrumb);
    return sentry_value_new_null();
}

// Test logger that outputs in a format the integration tests can parse
static void
test_logger_callback(
    sentry_level_t level, const char *message, va_list args, void *userdata)
{
    (void)level;
    (void)userdata;

    char formatted_message[1024];
    vsnprintf(formatted_message, sizeof(formatted_message), message, args);

    // Output in a format that the Python tests can detect
    printf("SENTRY_LOG:%s\n", formatted_message);
    fflush(stdout);
}

static void
print_envelope(sentry_envelope_t *envelope, void *unused_state)
{
    (void)unused_state;
    size_t size_out = 0;
    char *s = sentry_envelope_serialize(envelope, &size_out);
    printf("%s", s);
    sentry_free(s);
    sentry_envelope_free(envelope);
}

static bool
has_arg(int argc, char **argv, const char *arg)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], arg) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Builds a proxy URL from a scheme, optional credentials, host, and a port
 * read from an environment variable (with a fallback default).
 */
static void
make_proxy_url(char *buf, size_t buf_size, const char *scheme,
    const char *credentials, const char *host, const char *port_env_var,
    const char *default_port)
{
    const char *port = getenv(port_env_var);
    if (!port)
        port = default_port;

    if (credentials) {
        snprintf(
            buf, buf_size, "%s://%s@%s:%s", scheme, credentials, host, port);
    } else {
        snprintf(buf, buf_size, "%s://%s:%s", scheme, host, port);
    }
}

static const char *
get_arg_value(int argc, char **argv, const char *arg)
{
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], arg) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

#if defined(SENTRY_PLATFORM_WINDOWS) && !defined(__MINGW32__)                  \
    && !defined(__MINGW64__)

int
call_rffe_many_times()
{
    RaiseFailFastException(NULL, NULL, 0);
    RaiseFailFastException(NULL, NULL, 0);
    RaiseFailFastException(NULL, NULL, 0);
    RaiseFailFastException(NULL, NULL, 0);
    return 1;
}

typedef int (*crash_func)();

void
indirect_call(crash_func func)
{
    // This code always generates CFG guards.
    func();
}

static void
trigger_stack_buffer_overrun()
{
    // Call into the middle of the Crashy function.
    crash_func func = (crash_func)((uintptr_t)(call_rffe_many_times) + 16);
    __try {
        // Generates a STATUS_STACK_BUFFER_OVERRUN exception if CFG triggers.
        indirect_call(func);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // CFG fast fail should never be caught.
        printf(
            "If you see me, then CFG wasn't enabled (compile with /guard:cf)");
    }
    // Should only reach here if CFG is disabled.
    abort();
}

static void
trigger_fastfail_crash()
{
    // this bypasses WINDOWS SEH and will only be caught with the
    // crashpad/native WER module enabled
    RaiseFailFastException(NULL, NULL, 0);
}

#endif

#ifdef SENTRY_PLATFORM_AIX
// AIX has a null page mapped to the bottom of memory, which means null derefs
// don't segfault. try dereferencing the top of memory instead; the top nibble
// seems to be unusable.
static void *invalid_mem = (void *)0xFFFFFFFFFFFFFF9B; // -100 for memset
#else
static void *invalid_mem = (void *)1;
#endif

// Detect Address Sanitizer (works for both GCC and Clang)
#if defined(__SANITIZE_ADDRESS__)
#    define SENTRY_ASAN_ACTIVE 1
#elif defined(__has_feature)
#    if __has_feature(address_sanitizer)
#        define SENTRY_ASAN_ACTIVE 1
#    endif
#endif

static void
trigger_crash()
{
#ifdef SENTRY_ASAN_ACTIVE
    // Under ASAN, raise signal directly to bypass ASAN's memory interception.
    // ASAN intercepts memset and would abort before our signal handler runs.
    raise(SIGSEGV);
#else
    memset((char *)invalid_mem, 1, 100);
#endif
}

static void
trigger_stack_overflow()
{
    alloca(1024);
    trigger_stack_overflow();
}

static void
trigger_oom(void)
{
#ifdef SENTRY_PLATFORM_WINDOWS
    // Optionally limit process memory to trigger OOM faster.
    // Set SENTRY_TEST_OOM_LIMIT to the limit in MiB (e.g. "8192" for 8GB).
    // Without this, OOM exhausts all virtual memory which can take minutes.
    // Note: a Job Object limit only constrains user-mode allocations and
    // does not exhaust kernel pool memory like a real system-wide OOM would.
    const char *oom_limit = getenv("SENTRY_TEST_OOM_LIMIT");
    if (oom_limit) {
        unsigned long long limit = strtoull(oom_limit, NULL, 10) * 1024 * 1024;
        if (limit > 0 && limit <= SIZE_MAX) {
            HANDLE job = CreateJobObjectW(NULL, NULL);
            if (job) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = { 0 };
                info.BasicLimitInformation.LimitFlags
                    = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
                info.ProcessMemoryLimit = (size_t)limit;
                SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                    &info, sizeof(info));
                AssignProcessToJobObject(job, GetCurrentProcess());
            }
        }
    }
#endif

    size_t count = 1024;
    for (;;) {
        void *p = malloc(count);
        if (!p) {
#ifdef SENTRY_PLATFORM_WINDOWS
            RaiseException(0xc000, 0, 0, NULL);
#else
            *((volatile int *)3) = 1;
#endif
        }
        count *= 2;
    }
}

static sentry_value_t
create_debug_crumb(const char *message)
{
    sentry_value_t debug_crumb = sentry_value_new_breadcrumb("http", message);
    sentry_value_set_by_key(
        debug_crumb, "category", sentry_value_new_string("example!"));
    sentry_value_set_by_key(
        debug_crumb, "level", sentry_value_new_string("debug"));

    // extend the `http` crumb with (optional) data properties as documented
    // here:
    // https://develop.sentry.dev/sdk/event-payloads/breadcrumbs/#breadcrumb-types
    sentry_value_t http_data = sentry_value_new_object();
    sentry_value_set_by_key(http_data, "url",
        sentry_value_new_string("https://example.com/api/1.0/users"));
    sentry_value_set_by_key(
        http_data, "method", sentry_value_new_string("GET"));
    sentry_value_set_by_key(
        http_data, "status_code", sentry_value_new_int32(200));
    sentry_value_set_by_key(http_data, "reason", sentry_value_new_string("OK"));
    sentry_value_set_by_key(debug_crumb, "data", http_data);
    return debug_crumb;
}

#define NUM_THREADS 50
#define NUM_TELEMETRY 100 // how many telemetry calls each thread makes
#define TELEMETRY_SLEEP_MS 1 // time (in ms) between telemetry calls

#if defined(SENTRY_PLATFORM_WINDOWS)
#    define sleep_ms(MILLISECONDS) Sleep(MILLISECONDS)
#else
#    define sleep_ms(MILLISECONDS) usleep(MILLISECONDS * 1000)
#endif

#ifdef SENTRY_PLATFORM_WINDOWS
typedef DWORD(WINAPI *thread_func_t)(LPVOID);

DWORD WINAPI
log_thread_func(LPVOID lpParam)
{
    (void)lpParam;
    for (int i = 0; i < NUM_TELEMETRY; i++) {
        sentry_log_debug(
            "thread log %d on thread %lu", i, get_current_thread_id());
        sleep_ms(TELEMETRY_SLEEP_MS);
    }
    return 0;
}

DWORD WINAPI
metric_thread_func(LPVOID lpParam)
{
    (void)lpParam;
    for (int i = 0; i < NUM_TELEMETRY; i++) {
        sentry_metrics_count("thread.counter", 1, sentry_value_new_null());
        sleep_ms(TELEMETRY_SLEEP_MS);
    }
    return 0;
}

static void
run_threads(thread_func_t func)
{
    HANDLE threads[NUM_THREADS];
    for (int t = 0; t < NUM_THREADS; t++) {
        threads[t] = CreateThread(NULL, 0, func, NULL, 0, NULL);
    }
    WaitForMultipleObjects(NUM_THREADS, threads, TRUE, INFINITE);
    for (int t = 0; t < NUM_THREADS; t++) {
        CloseHandle(threads[t]);
    }
}
#else
typedef void *(*thread_func_t)(void *);

void *
log_thread_func(void *arg)
{
    (void)arg;
    for (int i = 0; i < NUM_TELEMETRY; i++) {
        sentry_log_debug(
            "thread log %d on thread %llu", i, get_current_thread_id());
        sleep_ms(TELEMETRY_SLEEP_MS);
    }
    return NULL;
}

void *
metric_thread_func(void *arg)
{
    (void)arg;
    for (int i = 0; i < NUM_TELEMETRY; i++) {
        sentry_metrics_count("thread.counter", 1, sentry_value_new_null());
        sleep_ms(TELEMETRY_SLEEP_MS);
    }
    return NULL;
}

static void
run_threads(thread_func_t func)
{
    pthread_t threads[NUM_THREADS];
    for (int t = 0; t < NUM_THREADS; t++) {
        pthread_create(&threads[t], NULL, func, NULL);
    }
    for (int t = 0; t < NUM_THREADS; t++) {
        pthread_join(threads[t], NULL);
    }
}
#endif

int
main(int argc, char **argv)
{
    sentry_options_t *options = sentry_options_new();

    if (has_arg(argc, argv, "disable-backend")) {
        sentry_options_set_backend(options, NULL);
    }

    // this is an example. for real usage, make sure to set this explicitly to
    // an app specific cache location.
    sentry_options_set_database_path(options, ".sentry-native");

    sentry_options_set_auto_session_tracking(options, false);
    sentry_options_set_symbolize_stacktraces(options, true);
    const char *shutdown_timeout
        = get_arg_value(argc, argv, "shutdown-timeout");
    if (shutdown_timeout) {
        sentry_options_set_shutdown_timeout(
            options, strtoull(shutdown_timeout, NULL, 10));
    }
    const char *transfer_timeout
        = get_arg_value(argc, argv, "transfer-timeout");
    if (transfer_timeout) {
        sentry_options_set_transfer_timeout(
            options, strtoull(transfer_timeout, NULL, 10));
    }

    sentry_options_set_environment(options, "development");
    // sentry defaults this to the `SENTRY_RELEASE` env variable
    if (!has_arg(argc, argv, "release-env")) {
        sentry_options_set_release(options, "test-example-release");
    }

    if (has_arg(argc, argv, "log")) {
        sentry_options_set_debug(options, 1);
    }

    if (has_arg(argc, argv, "attachment")) {
        // assuming the example / test is run directly from the cmake build
        // directory
        sentry_options_add_attachment(options, "./CMakeCache.txt");
    }

    if (has_arg(argc, argv, "large-attachment")) {
        sentry_options_set_enable_large_attachments(options, 1);
    }

    if (has_arg(argc, argv, "app-hang")) {
        sentry_options_set_enable_app_hang_tracking(options, 1);
        sentry_options_set_app_hang_timeout(options, 1000);
    }

    if (has_arg(argc, argv, "stdout")) {
        sentry_options_set_transport(
            options, sentry_transport_new(print_envelope));
    }

    if (has_arg(argc, argv, "capture-transaction")
        || has_arg(argc, argv, "open-transaction")) {
        sentry_options_set_traces_sample_rate(options, 1.0);
    }

    if (has_arg(argc, argv, "org-id")) {
        sentry_options_set_org_id(options, "123456");
    }

    if (has_arg(argc, argv, "strict-trace-continuation")) {
        sentry_options_set_strict_trace_continuation(options, 1);
    }

    if (has_arg(argc, argv, "child-spans")) {
        sentry_options_set_max_spans(options, 5);
    }

    if (has_arg(argc, argv, "before-send")) {
        sentry_options_set_before_send(options, before_send_callback, NULL);
    }

    if (has_arg(argc, argv, "discarding-before-send")) {
        sentry_options_set_before_send(
            options, discarding_before_send_callback, NULL);
    }

    if (has_arg(argc, argv, "on-crash")) {
        sentry_options_set_on_crash(options, on_crash_callback, NULL);
    }

    if (has_arg(argc, argv, "discarding-on-crash")) {
        sentry_options_set_on_crash(
            options, discarding_on_crash_callback, NULL);
    }

    if (has_arg(argc, argv, "restart-on-crash")) {
        sentry_options_set_on_crash(
            options, restart_on_crash, restart_args(argc, argv));
    }

    if (has_arg(argc, argv, "before-transaction")) {
        sentry_options_set_before_transaction(
            options, before_transaction_callback, NULL);
    }

    if (has_arg(argc, argv, "discarding-before-transaction")) {
        sentry_options_set_before_transaction(
            options, discarding_before_transaction_callback, NULL);
    }

    if (has_arg(argc, argv, "before-send-log")) {
        sentry_options_set_before_send_log(
            options, before_send_log_callback, NULL);
    }

    if (has_arg(argc, argv, "discarding-before-send-log")) {
        sentry_options_set_before_send_log(
            options, discarding_before_send_log_callback, NULL);
    }

    if (has_arg(argc, argv, "traces-sampler")) {
        sentry_options_set_traces_sampler(
            options, traces_sampler_callback, NULL);
    }

    if (has_arg(argc, argv, "override-sdk-name")) {
        sentry_options_set_sdk_name(options, "sentry.native.android.flutter");
    }

    if (has_arg(argc, argv, "http-proxy")) {
        char proxy_url[128];
        make_proxy_url(proxy_url, sizeof(proxy_url), "http", NULL, "127.0.0.1",
            "SENTRY_TEST_PROXY_PORT", "8080");
        sentry_options_set_proxy(options, proxy_url);
    }
    if (has_arg(argc, argv, "http-proxy-auth")) {
        char proxy_url[128];
        make_proxy_url(proxy_url, sizeof(proxy_url), "http", "user:password",
            "127.0.0.1", "SENTRY_TEST_PROXY_PORT", "8080");
        sentry_options_set_proxy(options, proxy_url);
    }
    if (has_arg(argc, argv, "http-proxy-ipv6")) {
        char proxy_url[128];
        make_proxy_url(proxy_url, sizeof(proxy_url), "http", NULL, "[::1]",
            "SENTRY_TEST_PROXY_PORT", "8080");
        sentry_options_set_proxy(options, proxy_url);
    }
    if (has_arg(argc, argv, "proxy-empty")) {
        sentry_options_set_proxy(options, "");
    }
    if (has_arg(argc, argv, "socks5-proxy")) {
        char proxy_url[128];
        make_proxy_url(proxy_url, sizeof(proxy_url), "socks5", NULL,
            "127.0.0.1", "SENTRY_TEST_PROXY_PORT", "1080");
        sentry_options_set_proxy(options, proxy_url);
    }

    if (has_arg(argc, argv, "crashpad-wait-for-upload")) {
        sentry_options_set_crashpad_wait_for_upload(options, true);
    }

    if (has_arg(argc, argv, "test-logger")) {
        // Set up the test logger for integration tests
        sentry_options_set_logger(options, test_logger_callback, NULL);
    }

    if (has_arg(argc, argv, "disable-logger-when-crashed")) {
        // Disable logging during crash handling
        sentry_options_set_logger_enabled_when_crashed(options, 0);
    }

    if (has_arg(argc, argv, "enable-logger-when-crashed")) {
        // Explicitly enable logging during crash handling (default behavior)
        sentry_options_set_logger_enabled_when_crashed(options, 1);
    }

    if (has_arg(argc, argv, "disable-logs")) {
        sentry_options_set_enable_logs(options, false);
    }

    if (has_arg(argc, argv, "crash-reporter")) {
#ifdef SENTRY_PLATFORM_WINDOWS
        sentry_options_set_external_crash_reporter_pathw(
            options, L"sentry_crash_reporter.exe");
#else
        sentry_options_set_external_crash_reporter_path(
            options, "./sentry_crash_reporter");
#endif
    }
    if (has_arg(argc, argv, "log-attributes")) {
        sentry_options_set_logs_with_attributes(options, true);
    }
    if (has_arg(argc, argv, "require-user-consent")) {
        sentry_options_set_require_user_consent(options, true);
    }
    if (has_arg(argc, argv, "cache-keep")
        || has_arg(argc, argv, "cache-keep-always")) {
        // true corresponds to SENTRY_CACHE_KEEP_OFFLINE for backwards
        // compatibility with older SDK versions that don't have the enum
        sentry_options_set_cache_keep(options, true);
        sentry_options_set_cache_max_size(options, 16 * 1024 * 1024); // 16 MB
        sentry_options_set_cache_max_age(options, 5 * 24 * 60 * 60); // 5 days
        sentry_options_set_cache_max_items(options, 5);
    }
    if (has_arg(argc, argv, "cache-keep-always")) {
        sentry_options_set_cache_keep(options, SENTRY_CACHE_KEEP_ALWAYS);
    }
    if (has_arg(argc, argv, "http-retry")) {
        sentry_options_set_http_retry(options, true);
    }
    if (has_arg(argc, argv, "no-http-retry")) {
        sentry_options_set_http_retry(options, false);
    }

    if (has_arg(argc, argv, "disable-metrics")) {
        sentry_options_set_enable_metrics(options, false);
    }

    if (has_arg(argc, argv, "before-send-metric")) {
        sentry_options_set_before_send_metric(
            options, before_send_metric_callback, NULL);
    }

    if (has_arg(argc, argv, "discarding-before-send-metric")) {
        sentry_options_set_before_send_metric(
            options, discarding_before_send_metric_callback, NULL);
    }

    if (has_arg(argc, argv, "before-breadcrumb")) {
        sentry_options_set_before_breadcrumb(
            options, before_breadcrumb_callback, NULL);
    }

    if (has_arg(argc, argv, "discarding-before-breadcrumb")) {
        sentry_options_set_before_breadcrumb(
            options, discarding_before_breadcrumb_callback, NULL);
    }

    if (has_arg(argc, argv, "crash-mode")) {
        const char *mode = get_arg_value(argc, argv, "crash-mode");
        if (mode != NULL) {
            if (strcmp(mode, "minidump") == 0) {
                sentry_options_set_crash_reporting_mode(
                    options, SENTRY_CRASH_REPORTING_MODE_MINIDUMP);
            } else if (strcmp(mode, "native") == 0) {
                sentry_options_set_crash_reporting_mode(
                    options, SENTRY_CRASH_REPORTING_MODE_NATIVE);
            } else if (strcmp(mode, "native-with-minidump") == 0) {
                sentry_options_set_crash_reporting_mode(
                    options, SENTRY_CRASH_REPORTING_MODE_NATIVE_WITH_MINIDUMP);
            }
        }
    }
    if (has_arg(argc, argv, "async-crash-upload")) {
        sentry_options_set_crash_upload_mode(
            options, SENTRY_CRASH_UPLOAD_MODE_ASYNC);
    }

    // E2E test mode: generate unique test ID for event correlation
    char e2e_test_id[37] = { 0 };
    if (has_arg(argc, argv, "e2e-test")) {
        sentry_uuid_t test_uuid = sentry_uuid_new_v4();
        sentry_uuid_as_string(&test_uuid, e2e_test_id);
    }

    if (0 != sentry_init(options)) {
        return EXIT_FAILURE;
    }

    if (has_arg(argc, argv, "user-consent-revoke")) {
        sentry_user_consent_revoke();
    }

    if (has_arg(argc, argv, "set-global-attribute")) {
        sentry_set_attribute("global.attribute.bool",
            sentry_value_new_attribute(sentry_value_new_bool(true), NULL));
        sentry_set_attribute("global.attribute.int",
            sentry_value_new_attribute(sentry_value_new_int64(123), NULL));
        sentry_set_attribute("global.attribute.double",
            sentry_value_new_attribute(sentry_value_new_double(1.23), NULL));
        sentry_set_attribute("global.attribute.string",
            sentry_value_new_attribute(
                sentry_value_new_string("my_global_value"), NULL));
        sentry_value_t array = sentry_value_new_list();
        sentry_value_append(array, sentry_value_new_string("item1"));
        sentry_value_append(array, sentry_value_new_string("item2"));
        sentry_set_attribute(
            "global.attribute.array", sentry_value_new_attribute(array, NULL));
        // same key as local attribute; value should be overwritten
        sentry_set_attribute("my.custom.attribute",
            sentry_value_new_attribute(
                sentry_value_new_string("my_global_value"), NULL));
    }

    // E2E test mode: set tags and output test ID for event correlation
    if (e2e_test_id[0] != '\0') {
        sentry_set_tag("test.id", e2e_test_id);
        sentry_set_tag("test.suite", "e2e");
        printf("TEST_ID:%s\n", e2e_test_id);
        fflush(stdout);
    }

    if (has_arg(argc, argv, "log-attributes")) {
        sentry_value_t attributes = sentry_value_new_object();
        sentry_value_t attr = sentry_value_new_attribute(
            sentry_value_new_string("my_attribute"), NULL);
        sentry_value_t attr_2 = sentry_value_new_attribute(
            sentry_value_new_int64(INT64_MAX), "fermions");
        sentry_value_t attr_3 = sentry_value_new_attribute(
            sentry_value_new_int64(INT64_MIN), "bosons");
        sentry_value_set_by_key(attributes, "my.custom.attribute", attr);
        sentry_value_set_by_key(attributes, "number.first", attr_2);
        sentry_value_set_by_key(attributes, "number.second", attr_3);
        // testing multiple attributes
        sentry_log_debug(
            "logging with %d custom attributes", attributes, (int64_t)3);
        // testing no attributes
        sentry_log_debug("logging with %s custom attributes",
            sentry_value_new_object(), "no");
        // testing overwriting default attributes
        sentry_value_t param_attributes = sentry_value_new_object();
        sentry_value_t param_attr = sentry_value_new_attribute(
            sentry_value_new_string("parameter"), NULL);
        sentry_value_t param_attr_2 = sentry_value_new_attribute(
            sentry_value_new_string("custom-sdk-name"), NULL);
        sentry_value_set_by_key(
            param_attributes, "sentry.message.parameter.0", param_attr);
        sentry_value_set_by_key(
            param_attributes, "sentry.sdk.name", param_attr_2);
        sentry_log_fatal(
            "logging with a custom parameter attribute", param_attributes);
    }

    if (has_arg(argc, argv, "attachment")) {
        sentry_attachment_t *bytes
            = sentry_attach_bytes("\xc0\xff\xee", 3, "bytes.bin");
        sentry_attachment_set_content_type(bytes, "application/octet-stream");
    }
    if (has_arg(argc, argv, "attach-view-hierarchy")) {
        // assuming the example / test is run directly from the cmake build
        // directory
        sentry_attachment_t *view_hierarchy
            = sentry_attach_file("./view-hierarchy.json");
        sentry_attachment_set_type(
            view_hierarchy, SENTRY_ATTACHMENT_TYPE_VIEW_HIERARCHY);
    }

    if (has_arg(argc, argv, "large-attachment")) {
        const char *large_file = ".sentry-large-attachment";
        FILE *f = fopen(large_file, "wb");
        if (f) {
            // 100 MB = TUS upload threshold
            char zeros[4096];
            memset(zeros, 0, sizeof(zeros));
            size_t remaining = 100 * 1024 * 1024;
            while (remaining > 0) {
                size_t chunk
                    = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
                fwrite(zeros, 1, chunk, f);
                remaining -= chunk;
            }
            fclose(f);
            sentry_attachment_t *attachment = sentry_attach_file(large_file);
            if (attachment) {
                sentry_attachment_set_type(
                    attachment, SENTRY_ATTACHMENT_TYPE_MINIDUMP);
            }
        }
    }

    if (sentry_options_get_enable_logs(options)) {
        if (has_arg(argc, argv, "capture-log")) {
            sentry_log_debug("I'm a log message!");
        }
        if (has_arg(argc, argv, "logs-timer")) {
            for (int i = 0; i < 10; i++) {
                sentry_log_info("Informational log nr.%d", i);
            }
            // sleep >5s to trigger logs timer
            sleep_s(6);
            // we should see two envelopes make its way to Sentry
            sentry_log_debug("post-sleep log");
        }
        if (has_arg(argc, argv, "logs-threads")) {
            run_threads(log_thread_func);
        }
    }

    if (sentry_options_get_enable_metrics(options)) {
        if (has_arg(argc, argv, "capture-metric")) {
            sentry_metrics_count("test.counter", 1, sentry_value_new_null());
        }
        if (has_arg(argc, argv, "capture-metric-all-types")) {
            sentry_metrics_count("test.counter", 1, sentry_value_new_null());
            sentry_metrics_gauge("test.gauge", 42.5, SENTRY_UNIT_PERCENT,
                sentry_value_new_null());
            sentry_metrics_distribution("test.distribution", 123.456,
                SENTRY_UNIT_MILLISECOND, sentry_value_new_null());
        }
        if (has_arg(argc, argv, "metric-with-attributes")) {
            sentry_value_t attributes = sentry_value_new_object();
            sentry_value_t attr = sentry_value_new_attribute(
                sentry_value_new_string("my_value"), NULL);
            sentry_value_set_by_key(attributes, "my.custom.attribute", attr);
            sentry_metrics_count("test.counter.with.attributes", 1, attributes);
        }
        if (has_arg(argc, argv, "metrics-timer")) {
            for (int i = 0; i < 10; i++) {
                sentry_metrics_count(
                    "batch.counter", 1, sentry_value_new_null());
            }
            sleep_s(6);
            sentry_metrics_count(
                "post.sleep.counter", 1, sentry_value_new_null());
        }
        if (has_arg(argc, argv, "metrics-threads")) {
            run_threads(metric_thread_func);
        }
    }

    if (!has_arg(argc, argv, "no-setup")) {
        sentry_set_transaction("test-transaction");
        sentry_set_level(SENTRY_LEVEL_WARNING);
        sentry_set_extra("extra stuff", sentry_value_new_string("some value"));
        sentry_set_extra("…unicode key…",
            // https://xkcd.com/1813/ :-)
            sentry_value_new_string("őá…–🤮🚀¿ 한글 테스트"));
        sentry_set_tag("expected-tag", "some value");
        sentry_set_tag("not-expected-tag", "some value");
        sentry_remove_tag("not-expected-tag");

        sentry_value_t context = sentry_value_new_object();
        sentry_value_set_by_key(
            context, "type", sentry_value_new_string("runtime"));
        sentry_value_set_by_key(
            context, "name", sentry_value_new_string("testing-runtime"));
        sentry_set_context("runtime", context);

        sentry_set_user(sentry_value_new_user(NULL, "some_name", NULL, NULL));

        sentry_value_t default_crumb
            = sentry_value_new_breadcrumb(NULL, "default level is info");
        sentry_add_breadcrumb(default_crumb);

        sentry_value_t debug_crumb = create_debug_crumb("debug crumb");
        sentry_add_breadcrumb(debug_crumb);

        sentry_value_t nl_crumb
            = sentry_value_new_breadcrumb(NULL, "lf\ncrlf\r\nlf\n...");
        sentry_value_set_by_key(
            nl_crumb, "category", sentry_value_new_string("something else"));
        sentry_add_breadcrumb(nl_crumb);
    }

    if (has_arg(argc, argv, "set-trace")) {
        const char *direct_trace_id = "aaaabbbbccccddddeeeeffff00001111";
        const char *direct_parent_span_id = "f0f0f0f0f0f0f0f0";
        sentry_set_trace(direct_trace_id, direct_parent_span_id);
    }

    if (has_arg(argc, argv, "replay-context")) {
        // mimics an embedder (e.g. sentry-unreal) that stages replay clips in
        // `<database>/replays/` and announces the active replay on the scope
        sentry_value_t replay = sentry_value_new_object();
        sentry_value_set_by_key(replay, "replay_id",
            sentry_value_new_string("deadbeefdeadbeefdeadbeefdeadbeef"));
        sentry_set_context("replay", replay);
    }

    if (has_arg(argc, argv, "attach-after-init")) {
        // assuming the example / test is run directly from the cmake build
        // directory
        sentry_attach_file("./CMakeCache.txt");
        sentry_attachment_t *bytes
            = sentry_attach_bytes("\xc0\xff\xee", 3, "bytes.bin");
        sentry_attachment_set_content_type(bytes, "application/octet-stream");
    }

    if (has_arg(argc, argv, "update-release-env")) {
        sentry_set_release("updated-release");
        sentry_set_environment("updated-environment");
    }

    if (has_arg(argc, argv, "start-session")) {
        sentry_start_session();
    }

    if (has_arg(argc, argv, "overflow-breadcrumbs")) {
        for (size_t i = 0; i < 101; i++) {
            char buffer[4];
            snprintf(buffer, 4, "%zu", i);
            sentry_add_breadcrumb(sentry_value_new_breadcrumb(0, buffer));
        }
    }

    if (has_arg(argc, argv, "clear-attachments")) {
        sentry_clear_attachments();
    }

    if (has_arg(argc, argv, "capture-with-scope")) {
        sentry_scope_t *scope = sentry_local_scope_new();

        sentry_value_t event = sentry_value_new_message_event(
            SENTRY_LEVEL_INFO, NULL, "Hello Scope!");

        sentry_value_t default_crumb
            = sentry_value_new_breadcrumb(NULL, "default level is info");
        sentry_scope_add_breadcrumb(scope, default_crumb);

        sentry_value_t debug_crumb = create_debug_crumb("scoped crumb");
        sentry_scope_add_breadcrumb(scope, debug_crumb);

        if (has_arg(argc, argv, "attach-to-scope")) {
            // assuming the example / test is run directly from the cmake build
            // directory
            sentry_scope_attach_file(scope, "./CMakeCache.txt");
            sentry_attachment_t *bytes = sentry_scope_attach_bytes(
                scope, "\xc0\xff\xee", 3, "bytes.bin");
            sentry_attachment_set_content_type(
                bytes, "application/octet-stream");
        }

        sentry_capture_event_with_scope(event, scope);
    }

    if (has_arg(argc, argv, "capture-multiple")) {
        for (size_t i = 0; i < 10; i++) {
            char buffer[10];
            snprintf(buffer, 10, "Event #%zu", i);

            sentry_value_t event = sentry_value_new_message_event(
                SENTRY_LEVEL_INFO, NULL, buffer);
            sentry_capture_event(event);
        }
    }

    if (has_arg(argc, argv, "reinstall")) {
        sentry_reinstall_backend();
    }

    if (has_arg(argc, argv, "flush")) {
        sentry_flush(10000);
    }

    if (has_arg(argc, argv, "sleep")) {
        sleep_s(10);
    }

    if (has_arg(argc, argv, "app-hang")) {
        printf("app-hang: start\n");
        fflush(stdout);

        // A couple of heartbeats to latch this (main) thread as the monitored
        // thread and keep it fresh.
        for (int i = 0; i < 3; i++) {
            sentry_app_hang_heartbeat();
            sleep_ms(100);
        }

        printf("app-hang: doing some heavy work now (going to sleep)\n");
        fflush(stdout);

        // Block the monitored thread past the configured timeout so the
        // watchdog samples this hung thread and captures an AppHang event.
        sleep_s(3);

        printf("app-hang: finishing\n");
        fflush(stdout);
    }

    if (has_arg(argc, argv, "test-logger-before-crash")) {
        // Output marker directly using printf for test parsing
        printf("pre-crash-log-message\n");
        fflush(stdout);
    }

    if (has_arg(argc, argv, "open-transaction")) {
        // Leave a transaction + nested children unfinished; the crash
        // auto-finalize should close them all.
        sentry_transaction_context_t *otx_ctx
            = sentry_transaction_context_new("open.tx", "op");
        sentry_transaction_t *otx
            = sentry_transaction_start(otx_ctx, sentry_value_new_null());
        sentry_set_transaction_object(otx);
        sentry_span_t *ospan
            = sentry_transaction_start_child(otx, "open.span", NULL);
        sentry_set_span(
            sentry_span_start_child(ospan, "open.grand.span", NULL));
    }

    if (has_arg(argc, argv, "crash")) {
        trigger_crash();
    }
    if (has_arg(argc, argv, "stack-overflow")) {
        trigger_stack_overflow();
    }
    if (has_arg(argc, argv, "oom")) {
        trigger_oom();
    }
#if defined(SENTRY_PLATFORM_WINDOWS) && !defined(__MINGW32__)                  \
    && !defined(__MINGW64__)
    if (has_arg(argc, argv, "fastfail")) {
        trigger_fastfail_crash();
    }
    if (has_arg(argc, argv, "stack-buffer-overrun")) {
        trigger_stack_buffer_overrun();
    }
#endif
    if (has_arg(argc, argv, "assert")) {
        assert(0);
    }
    if (has_arg(argc, argv, "abort")) {
#ifdef _WIN32
        // Suppress the Windows abort dialog that would block CI
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        abort();
    }
#ifdef SENTRY_PLATFORM_UNIX
    if (has_arg(argc, argv, "raise")) {
        raise(SIGSEGV);
    }
    if (has_arg(argc, argv, "kill")) {
        kill(getpid(), SIGSEGV);
    }
#endif

    if (has_arg(argc, argv, "capture-event")) {
        sentry_value_t event = sentry_value_new_message_event(
            SENTRY_LEVEL_INFO, "my-logger", "Hello World!");
        if (has_arg(argc, argv, "add-stacktrace")) {
            sentry_value_t thread
                = sentry_value_new_thread(get_current_thread_id(), "main");
            sentry_value_set_stacktrace(thread, NULL, 0);
            sentry_event_add_thread(event, thread);
        }
        sentry_capture_event(event);
    }
    if (has_arg(argc, argv, "user-consent-give")) {
        sentry_user_consent_give();
        sentry_flush(10000);
    }
    if (has_arg(argc, argv, "capture-exception")) {
        sentry_value_t exc = sentry_value_new_exception(
            "ParseIntError", "invalid digit found in string");
        if (has_arg(argc, argv, "add-stacktrace")) {
            sentry_value_set_stacktrace(exc, NULL, 0);
        }
        sentry_value_t event = sentry_value_new_event();
        sentry_event_add_exception(event, exc);

        sentry_capture_event(event);
    }
    if (has_arg(argc, argv, "capture-user-feedback")) {
        sentry_value_t user_feedback = sentry_value_new_feedback(
            "some-message", "some-email", "some-name", NULL);

        sentry_capture_feedback(user_feedback);
    }
    if (has_arg(argc, argv, "capture-user-feedback-with-attachment")) {
        sentry_value_t user_feedback = sentry_value_new_feedback(
            "some-message", "some-email", "some-name", NULL);

        // Create a hint and attach both file and byte data
        sentry_hint_t *hint = sentry_hint_new();

        // Create a temporary file for the attachment
        const char *attachment_path = ".sentry-test-feedback-attachment";
        FILE *f = fopen(attachment_path, "w");
        if (f) {
            fprintf(f, "This is feedback attachment content");
            fclose(f);
        }

        // Attach a file
        sentry_hint_attach_file(hint, attachment_path);

        // Attach bytes data (e.g., binary data from memory)
        const char *binary_data = "binary attachment data";
        sentry_hint_attach_bytes(
            hint, binary_data, strlen(binary_data), "additional-info.txt");

        // Capture feedback with attachments
        sentry_capture_feedback_with_hint(user_feedback, hint);

        // Clean up the temporary file
        remove(attachment_path);
    }
    if (has_arg(argc, argv, "capture-user-report")) {
        sentry_value_t event = sentry_value_new_message_event(
            SENTRY_LEVEL_INFO, "my-logger", "Hello user feedback!");
        sentry_uuid_t event_id = sentry_capture_event(event);

        SENTRY_SUPPRESS_DEPRECATED
        sentry_value_t user_feedback = sentry_value_new_user_feedback(
            &event_id, "some-name", "some-email", "some-comment");

        sentry_capture_user_feedback(user_feedback);
        SENTRY_RESTORE_DEPRECATED
    }

    if (has_arg(argc, argv, "capture-transaction")) {
        sentry_transaction_context_t *tx_ctx
            = sentry_transaction_context_new("little.teapot",
                "Short and stout here is my handle and here is my spout");

        if (has_arg(argc, argv, "unsample-tx")) {
            sentry_transaction_context_set_sampled(tx_ctx, 0);
        }

        sentry_value_t custom_sampling_ctx = sentry_value_new_object();
        sentry_value_set_by_key(
            custom_sampling_ctx, "b", sentry_value_new_int32(42));

        if (has_arg(argc, argv, "update-tx-from-header")) {
            const char *trace_header
                = "2674eb52d5874b13b560236d6c79ce8a-a0f9fdf04f1a63df";
            sentry_transaction_context_update_from_header(
                tx_ctx, "sentry-trace", trace_header);
        }
        if (has_arg(argc, argv, "incoming-trace")) {
            sentry_transaction_context_update_from_header(tx_ctx,
                "sentry-trace",
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-1");
        }
        if (has_arg(argc, argv, "incoming-baggage")) {
            sentry_transaction_context_update_from_header(tx_ctx, "baggage",
                "sentry-org_id=123456,sentry-environment=upstream,"
                "sentry-release=upstream-app%401.0");
        }
        if (has_arg(argc, argv, "incoming-baggage-mismatch")) {
            sentry_transaction_context_update_from_header(tx_ctx, "baggage",
                "sentry-org_id=99999,sentry-environment=upstream");
        }

        sentry_transaction_t *tx
            = sentry_transaction_start(tx_ctx, custom_sampling_ctx);

        sentry_transaction_set_data(
            tx, "url", sentry_value_new_string("https://example.com"));

        if (has_arg(argc, argv, "error-status")) {
            sentry_transaction_set_status(
                tx, SENTRY_SPAN_STATUS_INTERNAL_ERROR);
        }

        if (has_arg(argc, argv, "child-spans")) {
            sentry_span_t *child
                = sentry_transaction_start_child(tx, "littler.teapot", NULL);
            sentry_span_t *grandchild
                = sentry_span_start_child(child, "littlest.teapot", NULL);

            sentry_span_set_data(
                child, "span_data_says", sentry_value_new_string("hi!"));

            if (has_arg(argc, argv, "error-status")) {
                sentry_span_set_status(child, SENTRY_SPAN_STATUS_NOT_FOUND);
                sentry_span_set_status(
                    grandchild, SENTRY_SPAN_STATUS_ALREADY_EXISTS);
            }

            sentry_span_finish(grandchild);
            sentry_span_finish(child);
        }

        if (has_arg(argc, argv, "scope-transaction-event")) {
            sentry_set_transaction_object(tx);
            sentry_value_t event = sentry_value_new_message_event(
                SENTRY_LEVEL_INFO, "my-logger", "Hello World!");
            sentry_capture_event(event);
            if (has_arg(argc, argv, "logs-scoped-transaction")) {
                sentry_log_debug("logging during scoped transaction event");
            }
            if (has_arg(argc, argv, "metrics-scoped-transaction")) {
                sentry_metrics_count(
                    "scoped.transaction.metric", 1, sentry_value_new_null());
            }
        }

        sentry_transaction_finish(tx);
        if (has_arg(argc, argv, "logs-scoped-transaction")) {
            sentry_log_debug("logging after scoped transaction event");
        }
    }

    if (has_arg(argc, argv, "capture-minidump")) {
        sentry_capture_minidump("minidump.dmp");
    }

    // make sure everything flushes
    sentry_close();

    if (has_arg(argc, argv, "sleep-after-shutdown")) {
        sleep_s(5);
    }

    if (has_arg(argc, argv, "crash-after-shutdown")) {
        trigger_crash();
    }

    return EXIT_SUCCESS;
}
