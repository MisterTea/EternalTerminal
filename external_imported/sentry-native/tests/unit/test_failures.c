#include "sentry_core.h"
#include "sentry_options.h"
#include "sentry_testsupport.h"

static int
transport_startup_fail(
    const sentry_options_t *UNUSED(options), void *UNUSED(state))
{
    return 1;
}

static void
noop_send(const sentry_envelope_t *UNUSED(envelope), void *UNUSED(data))
{
}

SENTRY_TEST(init_failure)
{
    sentry_transport_t *transport
        = sentry_new_function_transport(noop_send, NULL);
    TEST_ASSERT(!!transport);
    sentry_transport_set_startup_func(transport, transport_startup_fail);

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_transport(options, transport);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    int rv = sentry_init(options);

#ifdef SENTRY_PLATFORM_NX
    // On NX a failing transport must not fail initialization.
    TEST_CHECK(rv == 0);
    SENTRY_WITH_OPTIONS (runtime_options) {
        TEST_CHECK(runtime_options->transport == NULL);
    }
#else
    TEST_CHECK(rv != 0);
#endif
}
