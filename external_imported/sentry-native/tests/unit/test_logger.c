#include "sentry_core.h"
#include "sentry_logger.h"
#include "sentry_testsupport.h"
#include <sentry.h>

typedef struct {
    uint64_t called;
    bool assert_now;
} logger_test_t;

static void
test_logger(
    sentry_level_t level, const char *message, va_list args, void *_data)
{
    logger_test_t *data = _data;
    if (data->assert_now) {
        data->called += 1;

        TEST_CHECK(level == SENTRY_LEVEL_WARNING);

        char formatted[128];
        vsprintf(formatted, message, args);

        TEST_CHECK_STRING_EQUAL(formatted, "Oh this is bad");
    }
}

SENTRY_TEST(custom_logger)
{
    logger_test_t data = { 0, false };

    sentry_options_t *options = sentry_options_new();
    sentry_options_set_debug(options, true);
    sentry_options_set_logger(options, test_logger, &data);

    sentry_init(options);

    data.assert_now = true;
    SENTRY_WARNF("Oh this is %s", "bad");
    data.assert_now = false;

    sentry_shutdown();

    TEST_CHECK_INT_EQUAL(data.called, 1);

    // *really* clear the logger instance
    options = sentry_options_new();
    sentry_init(options);
    sentry_shutdown();
}
