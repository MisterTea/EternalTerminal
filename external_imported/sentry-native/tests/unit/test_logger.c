#include "sentry_core.h"
#include "sentry_logger.h"
#include "sentry_sync.h"
#include "sentry_testsupport.h"

typedef struct {
    uint64_t called;
    bool assert_now;
} logger_test_t;

// Note: All logger unit-tests must only run from the transportless unit-test
// suite, since the transport can concurrently log while we do our
// single-threaded test assertions here, leading to flaky test runs.
// To blacklist a test, add to the respective list of `test_unit_transport`
// in the `tests/test_unit.py` unit-test runner.

static void
test_logger(
    sentry_level_t level, const char *message, va_list args, void *_data)
{
    logger_test_t *data = _data;
    if (data->assert_now) {
        data->called += 1;

        TEST_CHECK(level == SENTRY_LEVEL_WARNING);

        char formatted[128];
        vsnprintf(formatted, sizeof(formatted), message, args);

        TEST_CHECK_STRING_EQUAL(formatted, "Oh this is bad");
    }
}

SENTRY_TEST(custom_logger)
{
    logger_test_t data = { 0, false };

    {
        SENTRY_TEST_OPTIONS_NEW(options);
        sentry_options_set_debug(options, true);
        sentry_options_set_logger(options, test_logger, &data);
        sentry_options_set_enable_metrics(options, false);
        sentry_options_set_enable_logs(options, false);

        sentry_init(options);

        data.assert_now = true;
        SENTRY_WARNF("Oh this is %s", "bad");
        data.assert_now = false;

        sentry_close();
    }

    TEST_CHECK_INT_EQUAL(data.called, 1);

    {
        // *really* clear the logger instance
        SENTRY_TEST_OPTIONS_NEW(options);
        sentry_init(options);
        sentry_close();
    }
}

SENTRY_TEST(logger_enable_disable_functionality)
{
    logger_test_t data = { 0, false };

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_debug(options, true);
    sentry_options_set_logger(options, test_logger, &data);
    sentry_options_set_enable_metrics(options, false);
    sentry_options_set_enable_logs(options, false);

    sentry_init(options);

    // Test logging is enabled by default
    data.called = 0;
    data.assert_now = true;
    SENTRY_WARNF("Oh this is %s", "bad");
    TEST_CHECK_INT_EQUAL(data.called, 1);

    // Test disabling logging
    sentry__logger_disable();
    data.called = 0;
    data.assert_now = false;
    SENTRY_WARNF("Don't log %s", "this");
    TEST_CHECK_INT_EQUAL(data.called, 0);

    // Test re-enabling logging
    sentry__logger_enable();
    data.called = 0;
    data.assert_now = true;
    SENTRY_WARNF("Oh this is %s", "bad");
    TEST_CHECK_INT_EQUAL(data.called, 1);
    data.assert_now = false;

    // Clear the logger instance
    SENTRY_TEST_OPTIONS_NEW(clean_options);
    sentry_init(clean_options);
    sentry_close();
}

static void
test_log_level(
    sentry_level_t level, const char *message, va_list args, void *_data)
{
    (void)level;
    (void)message;
    (void)args;

    logger_test_t *data = _data;
    if (data->assert_now) {
        data->called += 1;
    }
}

SENTRY_TEST(logger_level)
{
    // Test structure: for each level, test that only messages >= that level are
    // logged
    const struct {
        sentry_level_t level;
        int expected_count; // How many of the 5 test messages should be logged
    } test_cases[] = {
        { SENTRY_LEVEL_TRACE,
            5 }, // All messages: TRACE, DEBUG, INFO, WARN, ERROR
        { SENTRY_LEVEL_DEBUG,
            4 }, // DEBUG, INFO, WARN, ERROR (TRACE filtered out)
        { SENTRY_LEVEL_INFO, 3 }, // INFO, WARN, ERROR
        { SENTRY_LEVEL_WARNING, 2 }, // WARN, ERROR
        { SENTRY_LEVEL_ERROR, 1 }, // ERROR only
    };

    for (size_t i = 0; i < 5; i++) { // for each of the 5 logger levels
        logger_test_t data = { 0, false };

        {
            SENTRY_TEST_OPTIONS_NEW(options);
            sentry_options_set_debug(options, true);
            sentry_options_set_logger_level(options, test_cases[i].level);
            sentry_options_set_logger(options, test_log_level, &data);
            sentry_options_set_enable_metrics(options, false);
            sentry_options_set_enable_logs(options, false);

            sentry_init(options);

            data.assert_now = true;
            // Test all 5 levels in order from most to least verbose
            SENTRY_TRACE("Logging Trace"); // level -2
            SENTRY_DEBUG("Logging Debug"); // level -1
            SENTRY_INFO("Logging Info"); // level 0
            SENTRY_WARN("Logging Warning"); // level 1
            SENTRY_ERROR("Logging Error"); // level 2

            data.assert_now = false;

            TEST_CHECK_INT_EQUAL(data.called, test_cases[i].expected_count);

            sentry_close();
        }

        {
            // *really* clear the logger instance
            SENTRY_TEST_OPTIONS_NEW(options);
            sentry_init(options);
            sentry_close();
        }
    }
}
