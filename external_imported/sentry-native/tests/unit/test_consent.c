#include "sentry_path.h"
#include "sentry_testsupport.h"

static void
init_consenting_sentry(void)
{
    sentry_options_t *opts = sentry_options_new();
    sentry_options_set_database_path(opts, SENTRY_TEST_PATH_PREFIX ".test-db");
    sentry_options_set_dsn(opts, "http://foo@127.0.0.1/42");
    sentry_options_set_require_user_consent(opts, true);
    sentry_init(opts);
}

static void
init_not_consenting_sentry(void)
{
    sentry_options_t *opts = sentry_options_new();
    sentry_options_set_database_path(opts, SENTRY_TEST_PATH_PREFIX ".test-db");
    sentry_options_set_dsn(opts, "http://foo@127.0.0.1/42");
    sentry_options_set_require_user_consent(opts, false);
    sentry_init(opts);
}

SENTRY_TEST(basic_consent_tracking)
{
    sentry_path_t *path
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".test-db");
    TEST_ASSERT(!!path);
    sentry__path_remove_all(path);

    init_consenting_sentry();
    TEST_CHECK_INT_EQUAL(
        sentry_user_consent_get(), SENTRY_USER_CONSENT_UNKNOWN);
    sentry_close();

    init_consenting_sentry();
    sentry_user_consent_give();
    // testing correct options ref/decref during double
    // `sentry_user_consent_give` call see
    // https://github.com/getsentry/sentry-native/pull/922
    sentry_user_consent_give();
    TEST_CHECK_INT_EQUAL(sentry_user_consent_get(), SENTRY_USER_CONSENT_GIVEN);
    sentry_close();
    init_consenting_sentry();
    TEST_CHECK_INT_EQUAL(sentry_user_consent_get(), SENTRY_USER_CONSENT_GIVEN);

    sentry_user_consent_revoke();
    TEST_CHECK_INT_EQUAL(
        sentry_user_consent_get(), SENTRY_USER_CONSENT_REVOKED);
    sentry_close();
    init_consenting_sentry();
    TEST_CHECK_INT_EQUAL(
        sentry_user_consent_get(), SENTRY_USER_CONSENT_REVOKED);

    sentry_user_consent_reset();
    TEST_CHECK_INT_EQUAL(
        sentry_user_consent_get(), SENTRY_USER_CONSENT_UNKNOWN);
    sentry_close();
    init_consenting_sentry();
    TEST_CHECK_INT_EQUAL(
        sentry_user_consent_get(), SENTRY_USER_CONSENT_UNKNOWN);
    sentry_close();

    sentry__path_remove_all(path);
    sentry__path_free(path);
}

SENTRY_TEST(query_consent_requirement)
{
    sentry_path_t *path
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".test-db");
    TEST_ASSERT(!!path);
    sentry__path_remove_all(path);

    // Test default behavior when require_user_consent is not set
    sentry_options_t *opts = sentry_options_new();
    sentry_options_set_database_path(opts, SENTRY_TEST_PATH_PREFIX ".test-db");
    sentry_options_set_dsn(opts, "http://foo@127.0.0.1/42");
    sentry_init(opts);
    TEST_CHECK_INT_EQUAL(sentry_user_consent_is_required(), 0);
    sentry_close();

    // Test when consent is explicitly NOT required
    init_not_consenting_sentry();
    TEST_CHECK_INT_EQUAL(sentry_user_consent_is_required(), 0);
    sentry_close();

    // Test when consent IS required
    init_consenting_sentry();
    TEST_CHECK_INT_EQUAL(sentry_user_consent_is_required(), 1);
    sentry_close();

    sentry__path_remove_all(path);
    sentry__path_free(path);
}
