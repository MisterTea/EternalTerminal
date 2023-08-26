#include "sentry_testsupport.h"

SENTRY_TEST(assert_sdk_version)
{
    TEST_CHECK_STRING_EQUAL(sentry_sdk_version(), SENTRY_SDK_VERSION);
}

SENTRY_TEST(assert_sdk_name)
{
    TEST_CHECK_STRING_EQUAL(sentry_sdk_name(), SENTRY_SDK_NAME);
}

SENTRY_TEST(assert_sdk_user_agent)
{
    TEST_CHECK_STRING_EQUAL(sentry_sdk_user_agent(), SENTRY_SDK_USER_AGENT);
}
