#include "sentry_options.h"
#include "sentry_testsupport.h"

#include <math.h>

SENTRY_TEST(options_sdk_name_defaults)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    // when nothing is set

    // then both sdk name and user agent should default to the build time
    // directives
    TEST_CHECK_STRING_EQUAL(
        sentry_options_get_sdk_name(options), SENTRY_SDK_NAME);
    TEST_CHECK_STRING_EQUAL(
        sentry_options_get_user_agent(options), SENTRY_SDK_USER_AGENT);

    sentry_options_free(options);
}

SENTRY_TEST(options_sdk_name_custom)
{
    SENTRY_TEST_OPTIONS_NEW(options);

    // when the sdk name is set to a custom string
    const int result
        = sentry_options_set_sdk_name(options, "sentry.native.android.flutter");

    // both the sdk_name and user_agent should reflect this change
    TEST_CHECK_INT_EQUAL(result, 0);
    TEST_CHECK_STRING_EQUAL(
        sentry_options_get_sdk_name(options), "sentry.native.android.flutter");

    TEST_CHECK_STRING_EQUAL(sentry_options_get_user_agent(options),
        "sentry.native.android.flutter/" SENTRY_SDK_VERSION);

    sentry_options_free(options);
}

SENTRY_TEST(options_sdk_name_invalid)
{
    SENTRY_TEST_OPTIONS_NEW(options);

    // when the sdk name is set to an invalid value
    const char *sdk_name = NULL;
    const int result = sentry_options_set_sdk_name(options, sdk_name);

    // then the value should be ignored
    TEST_CHECK_INT_EQUAL(result, 1);
    TEST_CHECK_STRING_EQUAL(
        sentry_options_get_sdk_name(options), SENTRY_SDK_NAME);
    TEST_CHECK_STRING_EQUAL(
        sentry_options_get_user_agent(options), SENTRY_SDK_USER_AGENT);

    sentry_options_free(options);
}

SENTRY_TEST(options_logger_enabled_when_crashed_default)
{
    SENTRY_TEST_OPTIONS_NEW(options);

    // Enabled by default
    TEST_CHECK_INT_EQUAL(options->enable_logging_when_crashed, 1);

    // Test setting to false
    sentry_options_set_logger_enabled_when_crashed(options, 0);
    TEST_CHECK_INT_EQUAL(options->enable_logging_when_crashed, 0);

    // Test setting to true
    sentry_options_set_logger_enabled_when_crashed(options, 1);
    TEST_CHECK_INT_EQUAL(options->enable_logging_when_crashed, 1);

    // Test setting with non-zero value (should be converted to 1)
    sentry_options_set_logger_enabled_when_crashed(options, 42);
    TEST_CHECK_INT_EQUAL(options->enable_logging_when_crashed, 1);

    sentry_options_free(options);
}

SENTRY_TEST(options_crash_reporting_mode_default)
{
    SENTRY_TEST_OPTIONS_NEW(options);

    // Default should be NATIVE_WITH_MINIDUMP (mode 3)
    TEST_CHECK_INT_EQUAL(sentry_options_get_crash_reporting_mode(options),
        SENTRY_CRASH_REPORTING_MODE_NATIVE_WITH_MINIDUMP);

    sentry_options_free(options);
}

SENTRY_TEST(options_crash_reporting_mode_set_get)
{
    SENTRY_TEST_OPTIONS_NEW(options);

    // Test setting to MINIDUMP mode
    sentry_options_set_crash_reporting_mode(
        options, SENTRY_CRASH_REPORTING_MODE_MINIDUMP);
    TEST_CHECK_INT_EQUAL(sentry_options_get_crash_reporting_mode(options),
        SENTRY_CRASH_REPORTING_MODE_MINIDUMP);

    // Test setting to NATIVE mode
    sentry_options_set_crash_reporting_mode(
        options, SENTRY_CRASH_REPORTING_MODE_NATIVE);
    TEST_CHECK_INT_EQUAL(sentry_options_get_crash_reporting_mode(options),
        SENTRY_CRASH_REPORTING_MODE_NATIVE);

    // Test setting to NATIVE_WITH_MINIDUMP mode
    sentry_options_set_crash_reporting_mode(
        options, SENTRY_CRASH_REPORTING_MODE_NATIVE_WITH_MINIDUMP);
    TEST_CHECK_INT_EQUAL(sentry_options_get_crash_reporting_mode(options),
        SENTRY_CRASH_REPORTING_MODE_NATIVE_WITH_MINIDUMP);

    sentry_options_free(options);
}

SENTRY_TEST(options_crash_reporting_mode_clamp)
{
    SENTRY_TEST_OPTIONS_NEW(options);

    // Test clamping invalid high values to NATIVE_WITH_MINIDUMP
    sentry_options_set_crash_reporting_mode(options, 99);
    TEST_CHECK_INT_EQUAL(sentry_options_get_crash_reporting_mode(options),
        SENTRY_CRASH_REPORTING_MODE_NATIVE_WITH_MINIDUMP);

    // Test clamping invalid low values to MINIDUMP
    sentry_options_set_crash_reporting_mode(options, -1);
    TEST_CHECK_INT_EQUAL(sentry_options_get_crash_reporting_mode(options),
        SENTRY_CRASH_REPORTING_MODE_MINIDUMP);

    sentry_options_free(options);
}

SENTRY_TEST(options_sample_rate)
{
    SENTRY_TEST_OPTIONS_NEW(options);

    sentry_options_set_sample_rate(options, 0.0);
    sentry_options_set_traces_sample_rate(options, 0.0);
    TEST_CHECK(sentry_options_get_sample_rate(options) == 0.0);
    TEST_CHECK(sentry_options_get_traces_sample_rate(options) == 0.0);

    sentry_options_set_sample_rate(options, 0.5);
    sentry_options_set_traces_sample_rate(options, 0.5);
    TEST_CHECK(sentry_options_get_sample_rate(options) == 0.5);
    TEST_CHECK(sentry_options_get_traces_sample_rate(options) == 0.5);

    sentry_options_set_sample_rate(options, 1.0);
    sentry_options_set_traces_sample_rate(options, 1.0);
    TEST_CHECK(sentry_options_get_sample_rate(options) == 1.0);
    TEST_CHECK(sentry_options_get_traces_sample_rate(options) == 1.0);

    // < 0.0
    sentry_options_set_sample_rate(options, -0.1);
    sentry_options_set_traces_sample_rate(options, -0.1);
    TEST_CHECK(sentry_options_get_sample_rate(options) == 0.0);
    TEST_CHECK(sentry_options_get_traces_sample_rate(options) == 0.0);

    // > 1.0
    sentry_options_set_sample_rate(options, 1.1);
    sentry_options_set_traces_sample_rate(options, 1.1);
    TEST_CHECK(sentry_options_get_sample_rate(options) == 1.0);
    TEST_CHECK(sentry_options_get_traces_sample_rate(options) == 1.0);

    // NaN -> default
    sentry_options_set_sample_rate(options, NAN);
    sentry_options_set_traces_sample_rate(options, NAN);
    TEST_CHECK(sentry_options_get_sample_rate(options) == 1.0);
    TEST_CHECK(sentry_options_get_traces_sample_rate(options) == 0.0);

    sentry_options_free(options);
}
