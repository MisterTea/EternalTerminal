#include "sentry_core.h"
#include "sentry_testsupport.h"

#include <sentry_sync.h>

static void
send_envelope_test_concurrent(const sentry_envelope_t *envelope, void *data)
{
    sentry__atomic_fetch_and_add((long *)data, 1);

    sentry_value_t event = sentry_envelope_get_event(envelope);
    if (!sentry_value_is_null(event)) {
        const char *event_id = sentry_value_as_string(
            sentry_value_get_by_key(event, "event_id"));
        TEST_CHECK_STRING_EQUAL(
            event_id, "4c035723-8638-4c3a-923f-2ab9d08b4018");
    }
}

static void
init_framework(long *called)
{
    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_transport(options,
        sentry_new_function_transport(send_envelope_test_concurrent, called));
    sentry_options_set_release(options, "prod");
    sentry_options_set_require_user_consent(options, false);
    sentry_options_set_auto_session_tracking(options, true);
    sentry_init(options);
}

SENTRY_TEST(multiple_inits)
{
    long called = 0;

    init_framework(&called);
    init_framework(&called);

    sentry_set_transaction("demo-trans");

    sentry_capture_event(sentry_value_new_message_event(
        SENTRY_LEVEL_INFO, "root", "Hello World!"));

    sentry_value_t obj = sentry_value_new_object();
    // something that is not a uuid, as this will be forcibly changed
    sentry_value_set_by_key(obj, "event_id", sentry_value_new_int32(1234));
    sentry_capture_event(obj);

    sentry_close();
    sentry_close();

    TEST_CHECK_INT_EQUAL(called, 4);
}

SENTRY_THREAD_FN
thread_worker(void *called)
{
    init_framework(called);

    sentry_set_transaction("demo-trans");

    sentry_capture_event(sentry_value_new_message_event(
        SENTRY_LEVEL_INFO, "root", "Hello World!"));

    sentry_value_t obj = sentry_value_new_object();
    // something that is not a uuid, as this will be forcibly changed
    sentry_value_set_by_key(obj, "event_id", sentry_value_new_int32(1234));
    sentry_capture_event(obj);

    return 0;
}

SENTRY_TEST(concurrent_init)
{
    long called = 0;

#define THREADS_NUM 10
    sentry_threadid_t threads[THREADS_NUM];

    for (size_t i = 0; i < THREADS_NUM; i++) {
        sentry__thread_init(&threads[i]);
        sentry__thread_spawn(&threads[i], &thread_worker, &called);
    }
    for (size_t i = 0; i < THREADS_NUM; i++) {
        sentry__thread_join(threads[i]);
        sentry__thread_free(&threads[i]);
    }

    sentry_close();

    // 1 session, and up to 2 events per thread.
    // might be less because `capture_event` races with close/init and might
    // lose events.
    TEST_CHECK(called >= THREADS_NUM * 1);
    TEST_CHECK(called <= THREADS_NUM * 3);
}

SENTRY_THREAD_FN
thread_breadcrumb(void *UNUSED(arg))
{
    sentry_value_t breadcrumb = sentry_value_new_breadcrumb("foo", "bar");
    sentry_add_breadcrumb(breadcrumb);

    return 0;
}

SENTRY_TEST(concurrent_uninit)
{
    sentry_value_t user = sentry_value_new_object();
    sentry_set_user(user);

    sentry_threadid_t thread;
    sentry__thread_init(&thread);
    sentry__thread_spawn(&thread, &thread_breadcrumb, NULL);

    sentry_value_t breadcrumb = sentry_value_new_breadcrumb("foo", "bar");
    sentry_add_breadcrumb(breadcrumb);

    sentry__thread_join(thread);
    sentry__thread_free(&thread);

    sentry_close();
}
