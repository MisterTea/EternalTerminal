#include "sentry_core.h"
#include "sentry_testsupport.h"

SENTRY_TEST(assert_sdk_version)
{
    TEST_CHECK_STRING_EQUAL(sentry_sdk_version(), SENTRY_SDK_VERSION);
}

SENTRY_TEST(assert_sdk_name)
{
    SENTRY_WITH_OPTIONS (options) {
        TEST_CHECK_STRING_EQUAL(
            sentry_options_get_sdk_name(options), SENTRY_SDK_NAME);
    }
}

SENTRY_TEST(assert_sdk_user_agent)
{
    SENTRY_WITH_OPTIONS (options) {
        TEST_CHECK_STRING_EQUAL(
            sentry_options_get_user_agent(options), SENTRY_SDK_USER_AGENT);
    }
}
