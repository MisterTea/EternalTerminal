#include "sentry_testsupport.h"
#include <sentry.h>

SENTRY_TEST(uninitialized)
{
    // make sure the public sentry API does not crash when called without a
    // `sentry_init`
    sentry_clear_modulecache();
    sentry_user_consent_give();
    sentry_user_consent_revoke();
    sentry_user_consent_reset();
    TEST_CHECK(sentry_user_consent_get() == SENTRY_USER_CONSENT_UNKNOWN);
    sentry_uuid_t uuid = sentry_capture_event(sentry_value_new_event());
    TEST_CHECK(sentry_uuid_is_nil(&uuid));
    sentry_handle_exception(NULL);
    sentry_add_breadcrumb(sentry_value_new_breadcrumb("foo", "bar"));
    sentry_set_user(sentry_value_new_object());
    sentry_remove_user();
    sentry_set_tag("foo", "bar");
    sentry_remove_tag("foo");
    sentry_set_extra("foo", sentry_value_new_null());
    sentry_remove_extra("foo");
    sentry_set_context("foo", sentry_value_new_object());
    sentry_remove_context("foo");
    sentry_set_fingerprint("foo", "bar", NULL);
    sentry_remove_fingerprint();
    sentry_set_transaction("foo");
    sentry_remove_transaction();
    sentry_set_level(SENTRY_LEVEL_DEBUG);
    sentry_start_session();
    sentry_end_session();
    sentry_shutdown();
}

SENTRY_TEST(empty_transport)
{
    sentry_options_t *options = sentry_options_new();
    sentry_options_set_transport(options, NULL);

    TEST_CHECK(sentry_init(options) == 0);

    sentry_value_t event = sentry_value_new_message_event(
        SENTRY_LEVEL_WARNING, NULL, "some message");
    sentry_uuid_t id = sentry_capture_event(event);
    TEST_CHECK(!sentry_uuid_is_nil(&id));

    sentry_shutdown();
}

SENTRY_TEST(invalid_dsn)
{
    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options, "not a valid dsn");

    TEST_CHECK(sentry_init(options) == 0);

    sentry_value_t event = sentry_value_new_message_event(
        SENTRY_LEVEL_WARNING, NULL, "some message");
    sentry_uuid_t id = sentry_capture_event(event);
    TEST_CHECK(!sentry_uuid_is_nil(&id));

    sentry_shutdown();
}

SENTRY_TEST(invalid_proxy)
{
    sentry_options_t *options = sentry_options_new();
    sentry_options_set_http_proxy(options, "invalid");

    TEST_CHECK(sentry_init(options) == 0);

    sentry_value_t event = sentry_value_new_message_event(
        SENTRY_LEVEL_WARNING, NULL, "some message");
    sentry_uuid_t id = sentry_capture_event(event);
    TEST_CHECK(!sentry_uuid_is_nil(&id));

    sentry_shutdown();
}
