#include "sentry_core.h"
#include "sentry_database.h"
#include "sentry_testsupport.h"

static void
send_envelope_test_basic(const sentry_envelope_t *envelope, void *data)
{
    uint64_t *called = data;
    *called += 1;

    sentry_value_t event = sentry_envelope_get_event(envelope);
    TEST_CHECK(!sentry_value_is_null(event));
    const char *event_id
        = sentry_value_as_string(sentry_value_get_by_key(event, "event_id"));
    TEST_CHECK_STRING_EQUAL(event_id, "4c035723-8638-4c3a-923f-2ab9d08b4018");

    if (*called == 1) {
        const char *msg = sentry_value_as_string(sentry_value_get_by_key(
            sentry_value_get_by_key(event, "message"), "formatted"));
        TEST_CHECK_STRING_EQUAL(msg, "Hello World!");
        const char *release
            = sentry_value_as_string(sentry_value_get_by_key(event, "release"));
        TEST_CHECK_STRING_EQUAL(release, "prod");
        const char *trans = sentry_value_as_string(
            sentry_value_get_by_key(event, "transaction"));
        TEST_CHECK_STRING_EQUAL(trans, "demo-trans");
    }
}

SENTRY_TEST(basic_function_transport)
{
    uint64_t called = 0;

    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_transport(options,
        sentry_new_function_transport(send_envelope_test_basic, &called));
    sentry_options_set_release(options, "prod");
    sentry_options_set_require_user_consent(options, true);
    sentry_init(options);

    sentry_set_transaction("demo-trans");

    sentry_capture_event(sentry_value_new_message_event(
        SENTRY_LEVEL_INFO, "root", "not captured due to missing consent"));
    sentry_user_consent_give();

    sentry_capture_event(sentry_value_new_message_event(
        SENTRY_LEVEL_INFO, "root", "Hello World!"));

    sentry_value_t obj = sentry_value_new_object();
    // something that is not a uuid, as this will be forcibly changed
    sentry_value_set_by_key(obj, "event_id", sentry_value_new_int32(1234));
    sentry_capture_event(obj);

    sentry_user_consent_revoke();
    sentry_capture_event(sentry_value_new_message_event(SENTRY_LEVEL_INFO,
        "root", "not captured either due to revoked consent"));

    sentry_close();

    TEST_CHECK_INT_EQUAL(called, 2);
}

static void
counting_transport_func(const sentry_envelope_t *UNUSED(envelope), void *data)
{
    uint64_t *called = data;
    *called += 1;
}

static sentry_value_t
before_send(sentry_value_t event, void *UNUSED(hint), void *data)
{
    uint64_t *called = data;
    *called += 1;

    return event;
}

SENTRY_TEST(sampling_before_send)
{
    uint64_t called_beforesend = 0;
    uint64_t called_transport = 0;

    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_transport(options,
        sentry_new_function_transport(
            counting_transport_func, &called_transport));
    sentry_options_set_before_send(options, before_send, &called_beforesend);
    sentry_options_set_sample_rate(options, 0.75);
    sentry_init(options);

    for (int i = 0; i < 100; i++) {
        sentry_capture_event(
            sentry_value_new_message_event(SENTRY_LEVEL_INFO, NULL, "foo"));
    }

    sentry_close();

    // The behavior here has changed with version 0.4.19:
    // the documentation (https://develop.sentry.dev/sdk/sessions/#filter-order)
    // requires that the sampling-rate filter for all SDKs is executed last.
    // This means the `before_send` callback will be invoked every time and only
    // the actual transport will be randomly sampled.
    TEST_CHECK(called_transport > 50 && called_transport < 100);
    TEST_CHECK_INT_EQUAL(called_beforesend, 100);
}

static sentry_value_t
discarding_before_send(sentry_value_t event, void *UNUSED(hint), void *data)
{
    uint64_t *called = data;
    *called += 1;

    sentry_value_decref(event);
    return sentry_value_new_null();
}

SENTRY_TEST(discarding_before_send)
{
    uint64_t called_beforesend = 0;
    uint64_t called_transport = 0;

    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_transport(options,
        sentry_new_function_transport(
            counting_transport_func, &called_transport));
    sentry_options_set_before_send(
        options, discarding_before_send, &called_beforesend);
    sentry_init(options);

    sentry_capture_event(
        sentry_value_new_message_event(SENTRY_LEVEL_INFO, NULL, "foo"));

    sentry_close();

    TEST_CHECK_INT_EQUAL(called_transport, 0);
    TEST_CHECK_INT_EQUAL(called_beforesend, 1);
}

SENTRY_TEST(crash_marker)
{
    sentry_options_t *options = sentry_options_new();

    // clear returns true, regardless if the file exists
    TEST_CHECK(sentry__clear_crash_marker(options));

    // write should normally be true, even when called multiple times
    TEST_CHECK(!sentry__has_crash_marker(options));
    TEST_CHECK(sentry__write_crash_marker(options));
    TEST_CHECK(sentry__has_crash_marker(options));
    TEST_CHECK(sentry__write_crash_marker(options));
    TEST_CHECK(sentry__has_crash_marker(options));

    TEST_CHECK(sentry__clear_crash_marker(options));
    TEST_CHECK(!sentry__has_crash_marker(options));
    TEST_CHECK(sentry__clear_crash_marker(options));

    sentry_options_free(options);
}

SENTRY_TEST(crashed_last_run)
{
    // fails before init() is called
    TEST_CHECK_INT_EQUAL(sentry_clear_crashed_last_run(), 1);

    // clear any leftover from previous test runs
    sentry_options_t *options = sentry_options_new();
    TEST_CHECK(sentry__clear_crash_marker(options));
    sentry_options_free(options);

    // -1 before sentry_init()
    TEST_CHECK_INT_EQUAL(sentry_get_crashed_last_run(), -1);

    options = sentry_options_new();
    const char *dsn_str = "https://foo@sentry.invalid/42";
    const char dsn[] = { 'h', 't', 't', 'p', 's', ':', '/', '/', 'f', 'o', 'o',
        '@', 's', 'e', 'n', 't', 'r', 'y', '.', 'i', 'n', 'v', 'a', 'l', 'i',
        'd', '/', '4', '2' };
    sentry_options_set_dsn_n(options, dsn, sizeof(dsn));
    TEST_CHECK_STRING_EQUAL(sentry_options_get_dsn(options), dsn_str);
    TEST_CHECK_INT_EQUAL(sentry_init(options), 0);
    sentry_close();

    TEST_CHECK_INT_EQUAL(sentry_get_crashed_last_run(), 0);

    options = sentry_options_new();
    sentry_options_set_dsn_n(options, dsn, sizeof(dsn));

    // simulate a crash
    TEST_CHECK(sentry__write_crash_marker(options));

    TEST_CHECK_INT_EQUAL(sentry_init(options), 0);

    TEST_CHECK_INT_EQUAL(sentry_get_crashed_last_run(), 1);

    // clear the status and re-init
    TEST_CHECK_INT_EQUAL(sentry_clear_crashed_last_run(), 0);

    sentry_close();

    // no change yet before sentry_init() is called
    TEST_CHECK_INT_EQUAL(sentry_get_crashed_last_run(), 1);

    options = sentry_options_new();
    sentry_options_set_dsn_n(options, dsn, sizeof(dsn));
    TEST_CHECK_INT_EQUAL(sentry_init(options), 0);
    sentry_close();

    TEST_CHECK_INT_EQUAL(sentry_get_crashed_last_run(), 0);
}
