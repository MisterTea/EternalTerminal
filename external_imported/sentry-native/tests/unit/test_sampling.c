#include "sentry_options.h"
#include "sentry_sampling_context.h"
#include "sentry_testsupport.h"
#include "sentry_tracing.h"

SENTRY_TEST(sampling_decision)
{
    TEST_CHECK(sentry__roll_dice(0.0) == false);
    TEST_CHECK(sentry__roll_dice(1.0));
    TEST_CHECK(sentry__roll_dice(2.0));
}

static double
traces_sampler_callback(const sentry_transaction_context_t *transaction_ctx,
    sentry_value_t custom_sampling_ctx, const int *parent_sampled,
    void *user_data)
{
    TEST_CHECK(user_data == (void *)0x12345678);

    const char *name = sentry_transaction_context_get_name(transaction_ctx);
    const char *operation
        = sentry_transaction_context_get_operation(transaction_ctx);

    if (strcmp(name, "skipme") == 0) {
        return 0.0;
    }
    if (strcmp(name, "sampleme") == 0) {
        return 1.0;
    }
    if (strcmp(operation, "skipme") == 0) {
        return 0.0;
    }
    if (strcmp(operation, "sampleme") == 0) {
        return 1.0;
    }
    if (parent_sampled != NULL) {
        if (*parent_sampled) {
            return 1; // high sample rate for children of sampled transactions
        }
        return 0; // parent is not sampled
    }
    if (sentry_value_as_int32(
            sentry_value_get_by_key(custom_sampling_ctx, "answer"))
        == 42) {
        return 1;
    }
    return 0;
}

SENTRY_TEST(sampling_transaction)
{
    {
        SENTRY_TEST_OPTIONS_NEW(options);
        TEST_CHECK(sentry_init(options) == 0);
    }

    sentry_transaction_context_t *tx_ctx
        = sentry_transaction_context_new("honk", NULL);

    sentry_transaction_context_set_sampled(tx_ctx, 0);
    sentry_sampling_context_t sampling_ctx
        = { tx_ctx, sentry_value_new_null(), NULL };
    TEST_CHECK(
        sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx) == false);

    sentry_transaction_context_set_sampled(tx_ctx, 1);
    TEST_CHECK(sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx));

    // fall back to default in sentry options (0.0) if sampled isn't there
    sentry_transaction_context_remove_sampled(tx_ctx);
    TEST_CHECK(
        sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx) == false);

    // sampled parent -> sampled child
    sentry_transaction_context_set_sampled(tx_ctx, 1);
    TEST_CHECK(sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx));

    {
        SENTRY_TEST_OPTIONS_NEW(options);
        sentry_options_set_traces_sample_rate(options, 1.0);
        TEST_CHECK(sentry_init(options) == 0);

        TEST_CHECK(
            sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx));

        // non-sampled parent
        sentry_transaction_context_set_sampled(tx_ctx, 0);
        TEST_CHECK(sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx)
            == false);

        sentry_transaction_context_remove_sampled(tx_ctx);
    }

    {
        // test the traces_sampler callback
        SENTRY_TEST_OPTIONS_NEW(options);
        sentry_options_set_traces_sampler(
            options, traces_sampler_callback, (void *)0x12345678);
        sentry_options_set_traces_sample_rate(options, 1.0);
        TEST_CHECK(sentry_init(options) == 0);

        sentry_value_t custom_sampling_ctx = sentry_value_new_object();
        sentry_value_set_by_key(
            custom_sampling_ctx, "answer", sentry_value_new_int32(42));
        sampling_ctx.custom_sampling_context = custom_sampling_ctx;

        TEST_CHECK(
            sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx));

        // non-sampled parent and traces sampler
        sentry_transaction_context_set_sampled(tx_ctx, 0);
        TEST_CHECK(sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx)
            == false);
        // removing sampled should fall back to traces sampler
        sentry_transaction_context_remove_sampled(tx_ctx);
        TEST_CHECK(
            sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx));
        sentry_value_set_by_key(
            custom_sampling_ctx, "answer", sentry_value_new_int32(21));
        TEST_CHECK(sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx)
            == false);

        // sampled parent and traces sampler
        sentry_transaction_context_set_sampled(tx_ctx, 1);
        TEST_CHECK(
            sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx));
        sentry_transaction_context_remove_sampled(tx_ctx);

        // testing transaction_context getters
        sentry_transaction_context_set_name(tx_ctx, "skipme");
        TEST_CHECK_STRING_EQUAL(
            sentry_transaction_context_get_name(tx_ctx), "skipme");
        TEST_CHECK(sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx)
            == false);
        sentry_transaction_context_set_name(tx_ctx, "sampleme");
        TEST_CHECK_STRING_EQUAL(
            sentry_transaction_context_get_name(tx_ctx), "sampleme");
        TEST_CHECK(
            sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx));
        sentry_transaction_context_set_name(tx_ctx, ""); // reset name

        sentry_transaction_context_set_operation(tx_ctx, "skipme");
        TEST_CHECK(sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx)
            == false);
        sentry_transaction_context_set_operation(tx_ctx, "sampleme");
        TEST_CHECK(
            sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx));

        // remove traces_sampler callback, should fall back to
        // traces_sample_rate
        options->traces_sampler = NULL;
        sentry_options_set_traces_sample_rate(options, 0.0);
        TEST_CHECK(sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx)
            == false);
        sentry_options_set_traces_sample_rate(options, 1.0);
        TEST_CHECK(
            sentry__should_send_transaction(tx_ctx->inner, &sampling_ctx));

        sentry__transaction_context_free(tx_ctx);
        sentry_value_decref(custom_sampling_ctx);
        sentry_close();
    }
}
