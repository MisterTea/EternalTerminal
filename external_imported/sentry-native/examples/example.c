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

#ifdef NDEBUG
#    undef NDEBUG
#endif

#include <assert.h>

#ifdef SENTRY_PLATFORM_WINDOWS
#    include <malloc.h>
#    include <synchapi.h>
#    define sleep_s(SECONDS) Sleep((SECONDS) * 1000)
#else

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
    sentry_value_t custom_sampling_ctx, const int *parent_sampled)
{
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
    // this bypasses WINDOWS SEH and will only be caught with the crashpad WER
    // module enabled
    __fastfail(77);
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

static void
trigger_crash()
{
    memset((char *)invalid_mem, 1, 100);
}

static void
trigger_stack_overflow()
{
    alloca(1024);
    trigger_stack_overflow();
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

    if (has_arg(argc, argv, "stdout")) {
        sentry_options_set_transport(
            options, sentry_transport_new(print_envelope));
    }

    if (has_arg(argc, argv, "capture-transaction")) {
        sentry_options_set_traces_sample_rate(options, 1.0);
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

    if (has_arg(argc, argv, "before-transaction")) {
        sentry_options_set_before_transaction(
            options, before_transaction_callback, NULL);
    }

    if (has_arg(argc, argv, "discarding-before-transaction")) {
        sentry_options_set_before_transaction(
            options, discarding_before_transaction_callback, NULL);
    }

    if (has_arg(argc, argv, "traces-sampler")) {
        sentry_options_set_traces_sampler(options, traces_sampler_callback);
    }

    if (has_arg(argc, argv, "override-sdk-name")) {
        sentry_options_set_sdk_name(options, "sentry.native.android.flutter");
    }

    if (has_arg(argc, argv, "http-proxy")) {
        sentry_options_set_proxy(options, "http://127.0.0.1:8080");
    }
    if (has_arg(argc, argv, "http-proxy-auth")) {
        sentry_options_set_proxy(
            options, "http://user:password@127.0.0.1:8080");
    }
    if (has_arg(argc, argv, "http-proxy-ipv6")) {
        sentry_options_set_proxy(options, "http://[::1]:8080");
    }
    if (has_arg(argc, argv, "proxy-empty")) {
        sentry_options_set_proxy(options, "");
    }

    if (has_arg(argc, argv, "socks5-proxy")) {
        sentry_options_set_proxy(options, "socks5://127.0.0.1:1080");
    }

    if (has_arg(argc, argv, "crashpad-wait-for-upload")) {
        sentry_options_set_crashpad_wait_for_upload(options, true);
    }

    if (has_arg(argc, argv, "attach-view-hierarchy")) {
        // assuming the example / test is run directly from the cmake build
        // directory
        sentry_options_add_view_hierarchy(options, "./view-hierarchy.json");
    }

    sentry_init(options);

    if (has_arg(argc, argv, "attachment")) {
        sentry_attachment_t *bytes
            = sentry_attach_bytes("\xc0\xff\xee", 3, "bytes.bin");
        sentry_attachment_set_content_type(bytes, "application/octet-stream");
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

        sentry_value_t user = sentry_value_new_object();
        sentry_value_set_by_key(user, "id", sentry_value_new_string("42"));
        sentry_value_set_by_key(
            user, "username", sentry_value_new_string("some_name"));
        sentry_set_user(user);

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

    if (has_arg(argc, argv, "attach-after-init")) {
        // assuming the example / test is run directly from the cmake build
        // directory
        sentry_attach_file("./CMakeCache.txt");
        sentry_attachment_t *bytes
            = sentry_attach_bytes("\xc0\xff\xee", 3, "bytes.bin");
        sentry_attachment_set_content_type(bytes, "application/octet-stream");
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

    if (has_arg(argc, argv, "sleep")) {
        sleep_s(10);
    }

    if (has_arg(argc, argv, "crash")) {
        trigger_crash();
    }
    if (has_arg(argc, argv, "stack-overflow")) {
        trigger_stack_overflow();
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
        sentry_value_t event = sentry_value_new_message_event(
            SENTRY_LEVEL_INFO, "my-logger", "Hello user feedback!");
        sentry_uuid_t event_id = sentry_capture_event(event);

        sentry_value_t user_feedback = sentry_value_new_user_feedback(
            &event_id, "some-name", "some-email", "some-comment");

        sentry_capture_user_feedback(user_feedback);
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
        }

        sentry_transaction_finish(tx);
    }

    if (has_arg(argc, argv, "capture-minidump")) {
        sentry_capture_minidump("minidump.dmp");
    }

    // make sure everything flushes
    sentry_close();

    if (has_arg(argc, argv, "sleep-after-shutdown")) {
        sleep_s(1);
    }

    if (has_arg(argc, argv, "crash-after-shutdown")) {
        trigger_crash();
    }
}
