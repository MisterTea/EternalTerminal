#include "sentry_envelope.h"
#include "sentry_session.h"
#include "sentry_testsupport.h"
#include "sentry_value.h"
#include <sentry.h>

static void
send_envelope(const sentry_envelope_t *envelope, void *data)
{
    uint64_t *called = data;
    *called += 1;

    TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(envelope), 1);

    const sentry_envelope_item_t *item = sentry__envelope_get_item(envelope, 0);
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry__envelope_item_get_header(item, "type")),
        "session");

    size_t buf_len;
    const char *buf = sentry__envelope_item_get_payload(item, &buf_len);
    sentry_value_t session = sentry__value_from_json(buf, buf_len);

    TEST_CHECK(sentry_value_is_true(sentry_value_get_by_key(session, "init")));
    TEST_CHECK_INT_EQUAL(
        sentry_value_get_type(sentry_value_get_by_key(session, "sid")),
        SENTRY_VALUE_TYPE_STRING);
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(session, "status")),
        "exited");
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(session, "did")),
        *called == 1 ? "foo@blabla.invalid" : "swatinem");
    TEST_CHECK_INT_EQUAL(
        sentry_value_as_int32(sentry_value_get_by_key(session, "errors")), 0);
    TEST_CHECK_INT_EQUAL(
        sentry_value_get_type(sentry_value_get_by_key(session, "started")),
        SENTRY_VALUE_TYPE_STRING);

    sentry_value_type_t duration_type
        = sentry_value_get_type(sentry_value_get_by_key(session, "duration"));
    TEST_CHECK(duration_type == SENTRY_VALUE_TYPE_DOUBLE
        || duration_type == SENTRY_VALUE_TYPE_INT32);

    sentry_value_t attrs = sentry_value_get_by_key(session, "attrs");
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(attrs, "release")),
        "my_release");
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(attrs, "environment")),
        "my_environment");

    sentry_value_decref(session);
}

SENTRY_TEST(session_basics)
{
    uint64_t called = 0;
    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_transport(
        options, sentry_new_function_transport(send_envelope, &called));
    sentry_options_set_release(options, "my_release");
    sentry_options_set_environment(options, "my_environment");
    sentry_init(options);

    // a session was already started by automatic session tracking
    sentry_value_t user = sentry_value_new_object();
    sentry_value_set_by_key(
        user, "email", sentry_value_new_string("foo@blabla.invalid"));
    sentry_set_user(user);

    sentry_end_session();
    sentry_start_session();

    user = sentry_value_new_object();
    sentry_value_set_by_key(
        user, "username", sentry_value_new_string("swatinem"));
    sentry_set_user(user);

    sentry_shutdown();

    TEST_CHECK_INT_EQUAL(called, 2);
}

typedef struct {
    bool assert_session;
    uint64_t called;
} session_assertion_t;

static void
send_sampled_envelope(const sentry_envelope_t *envelope, void *data)
{
    session_assertion_t *assertion = data;

    if (assertion->assert_session) {
        assertion->called += 1;

        TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(envelope), 1);

        const sentry_envelope_item_t *item
            = sentry__envelope_get_item(envelope, 0);
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(
                sentry__envelope_item_get_header(item, "type")),
            "session");

        size_t buf_len;
        const char *buf = sentry__envelope_item_get_payload(item, &buf_len);
        sentry_value_t session = sentry__value_from_json(buf, buf_len);

        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(session, "status")),
            "exited");
        TEST_CHECK_INT_EQUAL(
            sentry_value_as_int32(sentry_value_get_by_key(session, "errors")),
            100);

        sentry_value_decref(session);
    }
}

SENTRY_TEST(count_sampled_events)
{
    session_assertion_t assertion = { false, 0 };

    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_transport(options,
        sentry_new_function_transport(send_sampled_envelope, &assertion));
    sentry_options_set_release(options, "my_release");
    sentry_options_set_sample_rate(options, 0.5);
    sentry_init(options);

    for (int i = 0; i < 100; i++) {
        sentry_capture_event(
            sentry_value_new_message_event(SENTRY_LEVEL_ERROR, NULL, "foo"));
    }

    assertion.assert_session = true;
    sentry_shutdown();

    TEST_CHECK_INT_EQUAL(assertion.called, 1);
}
