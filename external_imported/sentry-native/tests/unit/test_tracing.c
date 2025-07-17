#include "sentry_testsupport.h"

#include "sentry_scope.h"
#include "sentry_string.h"
#include "sentry_tracing.h"
#include "sentry_uuid.h"

#define IS_NULL(Src, Field)                                                    \
    sentry_value_is_null(sentry_value_get_by_key(Src, Field))
#define CHECK_STRING_PROPERTY(Src, Field, Expected)                            \
    TEST_CHECK_STRING_EQUAL(                                                   \
        sentry_value_as_string(sentry_value_get_by_key(Src, Field)), Expected)

SENTRY_TEST(basic_tracing_context)
{
    sentry_transaction_t *opaque_tx
        = sentry__transaction_new(sentry_value_new_null());
    TEST_CHECK(!opaque_tx);

    sentry_value_t tx = sentry_value_new_object();
    opaque_tx = sentry__transaction_new(sentry__value_clone(tx));
    TEST_ASSERT(!!opaque_tx);
    sentry_value_set_by_key(tx, "op", sentry_value_new_string("honk.beep"));
    TEST_CHECK(sentry_value_is_null(
        sentry__value_get_trace_context(opaque_tx->inner)));

    sentry_uuid_t trace_id = sentry_uuid_new_v4();
    sentry_value_set_by_key(
        tx, "trace_id", sentry__value_new_internal_uuid(&trace_id));
    sentry__transaction_decref(opaque_tx);
    opaque_tx = sentry__transaction_new(sentry__value_clone(tx));
    TEST_ASSERT(!!opaque_tx);
    TEST_CHECK(sentry_value_is_null(
        sentry__value_get_trace_context(opaque_tx->inner)));

    sentry_uuid_t span_id = sentry_uuid_new_v4();
    sentry_value_set_by_key(
        tx, "span_id", sentry__value_new_span_uuid(&span_id));
    sentry__transaction_decref(opaque_tx);
    opaque_tx = sentry__transaction_new(sentry__value_clone(tx));

    sentry_value_t trace_context
        = sentry__value_get_trace_context(opaque_tx->inner);
    TEST_CHECK(!sentry_value_is_null(trace_context));
    TEST_CHECK(!IS_NULL(trace_context, "trace_id"));
    TEST_CHECK(!IS_NULL(trace_context, "span_id"));

    CHECK_STRING_PROPERTY(trace_context, "op", "honk.beep");

    sentry_value_decref(trace_context);
    sentry_value_decref(tx);
    sentry__transaction_decref(opaque_tx);
}

SENTRY_TEST(basic_transaction)
{
    sentry_transaction_context_t *opaque_tx_ctx
        = sentry_transaction_context_new(NULL, NULL);
    sentry_value_t tx_ctx;
    if (opaque_tx_ctx != NULL) {
        tx_ctx = opaque_tx_ctx->inner;
        TEST_CHECK(!sentry_value_is_null(tx_ctx));
        CHECK_STRING_PROPERTY(tx_ctx, "transaction", "");
        CHECK_STRING_PROPERTY(tx_ctx, "op", "");
        TEST_CHECK(!IS_NULL(tx_ctx, "trace_id"));
        TEST_CHECK(!IS_NULL(tx_ctx, "span_id"));
    } else {
        TEST_CHECK(opaque_tx_ctx != NULL);
    }

    sentry__transaction_context_free(opaque_tx_ctx);

    opaque_tx_ctx = sentry_transaction_context_new("", "");
    if (opaque_tx_ctx != NULL) {
        tx_ctx = opaque_tx_ctx->inner;
        TEST_CHECK(!sentry_value_is_null(tx_ctx));
        CHECK_STRING_PROPERTY(tx_ctx, "transaction", "");
        CHECK_STRING_PROPERTY(tx_ctx, "op", "");
        TEST_CHECK(!IS_NULL(tx_ctx, "trace_id"));
        TEST_CHECK(!IS_NULL(tx_ctx, "span_id"));
    } else {
        TEST_CHECK(opaque_tx_ctx != NULL);
    }

    sentry__transaction_context_free(opaque_tx_ctx);

    opaque_tx_ctx = sentry_transaction_context_new("honk.beep", "beepbeep");
    if (opaque_tx_ctx != NULL) {
        tx_ctx = opaque_tx_ctx->inner;
        TEST_CHECK(!sentry_value_is_null(tx_ctx));
        CHECK_STRING_PROPERTY(tx_ctx, "transaction", "honk.beep");
        CHECK_STRING_PROPERTY(tx_ctx, "op", "beepbeep");
        TEST_CHECK(!IS_NULL(tx_ctx, "trace_id"));
        TEST_CHECK(!IS_NULL(tx_ctx, "span_id"));

        sentry_transaction_context_set_name(opaque_tx_ctx, "");
        CHECK_STRING_PROPERTY(tx_ctx, "transaction", "");

        char txn_ctx_name[] = { 'h', 'o', 'n', 'k', '.', 'b', 'e', 'e', 'p' };
        sentry_transaction_context_set_name_n(
            opaque_tx_ctx, txn_ctx_name, sizeof(txn_ctx_name));
        CHECK_STRING_PROPERTY(tx_ctx, "transaction", "honk.beep");

        sentry_transaction_context_set_operation(opaque_tx_ctx, "");
        CHECK_STRING_PROPERTY(tx_ctx, "op", "");

        char txn_ctx_op[] = { 'b', 'e', 'e', 'p', 'b', 'e', 'e', 'p' };
        sentry_transaction_context_set_operation_n(
            opaque_tx_ctx, txn_ctx_op, sizeof(txn_ctx_op));
        CHECK_STRING_PROPERTY(tx_ctx, "op", "beepbeep");

        sentry_transaction_context_set_sampled(opaque_tx_ctx, 1);
        TEST_CHECK(
            sentry_value_is_true(sentry_value_get_by_key(tx_ctx, "sampled"))
            == 1);
    } else {
        TEST_CHECK(opaque_tx_ctx != NULL);
    }

    sentry__transaction_context_free(opaque_tx_ctx);
}

static void
check_backfilled_name(sentry_envelope_t *envelope, void *data)
{
    uint64_t *called = data;
    *called += 1;

    sentry_value_t tx = sentry_envelope_get_transaction(envelope);
    TEST_CHECK(!sentry_value_is_null(tx));
    CHECK_STRING_PROPERTY(tx, "transaction", "<unlabeled transaction>");

    sentry_envelope_free(envelope);
}

SENTRY_TEST(transaction_name_backfill_on_finish)
{
    uint64_t called = 0;

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    // Disable sessions or this test would fail if env:SENTRY_RELEASE is set.
    sentry_options_set_auto_session_tracking(options, 0);

    sentry_transport_t *transport = sentry_transport_new(check_backfilled_name);
    sentry_transport_set_state(transport, &called);
    sentry_options_set_transport(options, transport);

    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_init(options);

    sentry_transaction_context_t *tx_ctx
        = sentry_transaction_context_new(NULL, NULL);
    sentry_transaction_t *tx
        = sentry_transaction_start(tx_ctx, sentry_value_new_null());
    sentry_uuid_t event_id = sentry_transaction_finish(tx);
    TEST_CHECK(!sentry_uuid_is_nil(&event_id));

    tx_ctx = sentry_transaction_context_new("", "");
    tx = sentry_transaction_start(tx_ctx, sentry_value_new_null());
    event_id = sentry_transaction_finish(tx);
    TEST_CHECK(!sentry_uuid_is_nil(&event_id));

    sentry_close();
    TEST_CHECK_INT_EQUAL(called, 2);
}

static void
send_transaction_envelope_test_basic(sentry_envelope_t *envelope, void *data)
{
    uint64_t *called = data;
    *called += 1;

    sentry_value_t tx = sentry_envelope_get_transaction(envelope);
    TEST_CHECK(!sentry_value_is_null(tx));
    CHECK_STRING_PROPERTY(
        tx, "event_id", "4c035723-8638-4c3a-923f-2ab9d08b4018");

    if (*called != 1) {
        CHECK_STRING_PROPERTY(tx, "type", "transaction");
        CHECK_STRING_PROPERTY(tx, "transaction", "honk");
    }

    sentry_envelope_free(envelope);
}

void
run_basic_function_transport_transaction(bool timestamped)
{
    uint64_t called = 0;

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");

    sentry_transport_t *transport
        = sentry_transport_new(send_transaction_envelope_test_basic);
    sentry_transport_set_state(transport, &called);
    sentry_options_set_transport(options, transport);

    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_options_set_require_user_consent(options, true);
    sentry_init(options);

    sentry_transaction_context_t *tx_ctx = sentry_transaction_context_new(
        "How could you", "Don't capture this.");
    sentry_transaction_t *tx;
    // TODO: `sentry_capture_event` acts as if the event was sent if user
    //  consent was not given
    if (timestamped) {
        tx = sentry_transaction_start_ts(tx_ctx, sentry_value_new_null(), 1);
        CHECK_STRING_PROPERTY(
            tx->inner, "start_timestamp", "1970-01-01T00:00:00.000001Z");
        sentry_uuid_t event_id = sentry_transaction_finish_ts(tx, 2);
        TEST_CHECK(!sentry_uuid_is_nil(&event_id));
    } else {
        tx = sentry_transaction_start(tx_ctx, sentry_value_new_null());
        sentry_uuid_t event_id = sentry_transaction_finish(tx);
        TEST_CHECK(!sentry_uuid_is_nil(&event_id));
    }

    sentry_user_consent_give();
    char name[] = { 'h', 'o', 'n', 'k' };
    char op[] = { 'b', 'e', 'e', 'p' };
    tx_ctx
        = sentry_transaction_context_new_n(name, sizeof(name), op, sizeof(op));
    if (timestamped) {
        tx = sentry_transaction_start_ts(tx_ctx, sentry_value_new_null(), 3);
        CHECK_STRING_PROPERTY(
            tx->inner, "start_timestamp", "1970-01-01T00:00:00.000003Z");
    } else {
        tx = sentry_transaction_start(tx_ctx, sentry_value_new_null());
    }
    CHECK_STRING_PROPERTY(tx->inner, "transaction", "honk");
    CHECK_STRING_PROPERTY(tx->inner, "op", "beep");
    if (timestamped) {
        sentry_uuid_t event_id = sentry_transaction_finish_ts(tx, 4);
        TEST_CHECK(!sentry_uuid_is_nil(&event_id));
    } else {
        sentry_uuid_t event_id = sentry_transaction_finish(tx);
        TEST_CHECK(!sentry_uuid_is_nil(&event_id));
    }

    sentry_user_consent_revoke();
    tx_ctx = sentry_transaction_context_new(
        "How could you again", "Don't capture this either.");
    // TODO: `sentry_capture_event` acts as if the event was sent if user
    //  consent was not given
    if (timestamped) {
        tx = sentry_transaction_start_ts(tx_ctx, sentry_value_new_null(), 5);
        CHECK_STRING_PROPERTY(
            tx->inner, "start_timestamp", "1970-01-01T00:00:00.000005Z");
        sentry_uuid_t event_id = sentry_transaction_finish_ts(tx, 6);
        TEST_CHECK(!sentry_uuid_is_nil(&event_id));
    } else {
        tx = sentry_transaction_start(tx_ctx, sentry_value_new_null());
        sentry_uuid_t event_id = sentry_transaction_finish(tx);
        TEST_CHECK(!sentry_uuid_is_nil(&event_id));
    }

    sentry_close();

    TEST_CHECK_INT_EQUAL(called, 1);
}
SENTRY_TEST(basic_function_transport_transaction)
{
    run_basic_function_transport_transaction(false);
}

SENTRY_TEST(basic_function_transport_transaction_ts)
{
    run_basic_function_transport_transaction(true);
}

SENTRY_TEST(transport_sampling_transactions)
{
    uint64_t called_transport = 0;

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    // Disable sessions or this test would fail if env:SENTRY_RELEASE is set.
    sentry_options_set_auto_session_tracking(options, 0);

    sentry_transport_t *transport
        = sentry_transport_new(send_transaction_envelope_test_basic);
    sentry_transport_set_state(transport, &called_transport);
    sentry_options_set_transport(options, transport);

    sentry_options_set_traces_sample_rate(options, 0.75);
    sentry_init(options);

    uint64_t sent_transactions = 0;
    for (int i = 0; i < 100; i++) {
        sentry_transaction_context_t *tx_ctx
            = sentry_transaction_context_new("honk", "beep");
        sentry_transaction_t *tx
            = sentry_transaction_start(tx_ctx, sentry_value_new_null());
        sentry_uuid_t event_id = sentry_transaction_finish(tx);
        if (!sentry_uuid_is_nil(&event_id)) {
            sent_transactions += 1;
        }
    }

    sentry_close();

    // exact value is nondeterministic because of rng
    TEST_CHECK(called_transport > 50 && called_transport < 100);
    TEST_CHECK(called_transport == sent_transactions);
}

static sentry_value_t
before_send(sentry_value_t event, void *UNUSED(hint), void *data)
{
    uint64_t *called = data;
    *called += 1;

    sentry_value_decref(event);
    return sentry_value_new_null();
}

SENTRY_TEST(transactions_skip_before_send)
{
    uint64_t called_beforesend = 0;
    uint64_t called_transport = 0;

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    // Disable sessions or this test would fail if env:SENTRY_RELEASE is set.
    sentry_options_set_auto_session_tracking(options, 0);

    sentry_transport_t *transport
        = sentry_transport_new(send_transaction_envelope_test_basic);
    sentry_transport_set_state(transport, &called_transport);
    sentry_options_set_transport(options, transport);

    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_options_set_before_send(options, before_send, &called_beforesend);
    sentry_init(options);

    sentry_transaction_context_t *tx_ctx
        = sentry_transaction_context_new("honk", "beep");
    sentry_transaction_t *tx
        = sentry_transaction_start(tx_ctx, sentry_value_new_null());
    sentry_uuid_t event_id = sentry_transaction_finish(tx);
    TEST_CHECK(!sentry_uuid_is_nil(&event_id));

    sentry_close();

    TEST_CHECK_INT_EQUAL(called_transport, 1);
    TEST_CHECK_INT_EQUAL(called_beforesend, 0);
}

static void
before_transport(sentry_envelope_t *envelope, void *data)
{
    uint64_t *called = data;
    *called += 1;

    sentry_envelope_free(envelope);
}

SENTRY_TEST(multiple_transactions)
{
    uint64_t called_transport = 0;

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    // Disable sessions or this test would fail if env:SENTRY_RELEASE is set.
    sentry_options_set_auto_session_tracking(options, 0);

    sentry_transport_t *transport = sentry_transport_new(before_transport);
    sentry_transport_set_state(transport, &called_transport);
    sentry_options_set_transport(options, transport);

    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_init(options);

    sentry_transaction_context_t *tx_ctx
        = sentry_transaction_context_new("wow!", NULL);
    sentry_transaction_t *tx
        = sentry_transaction_start(tx_ctx, sentry_value_new_null());
    sentry_set_transaction_object(tx);

    sentry_value_t scope_tx = sentry__scope_get_span_or_transaction();
    CHECK_STRING_PROPERTY(scope_tx, "transaction", "wow!");

    sentry_uuid_t event_id = sentry_transaction_finish(tx);
    scope_tx = sentry__scope_get_span_or_transaction();
    TEST_CHECK(sentry_value_is_null(scope_tx));
    TEST_CHECK(!sentry_uuid_is_nil(&event_id));

    // Set transaction on scope twice, back-to-back without finishing the first
    // one
    tx_ctx = sentry_transaction_context_new("whoa!", NULL);
    tx = sentry_transaction_start(tx_ctx, sentry_value_new_null());
    sentry_set_transaction_object(tx);
    sentry__transaction_decref(tx);
    tx_ctx = sentry_transaction_context_new("wowee!", NULL);
    tx = sentry_transaction_start(tx_ctx, sentry_value_new_null());
    sentry_set_transaction_object(tx);
    scope_tx = sentry__scope_get_span_or_transaction();
    CHECK_STRING_PROPERTY(scope_tx, "transaction", "wowee!");
    event_id = sentry_transaction_finish(tx);
    TEST_CHECK(!sentry_uuid_is_nil(&event_id));

    sentry_close();

    TEST_CHECK_INT_EQUAL(called_transport, 2);
}

SENTRY_TEST(basic_spans)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_init(options);

    // Starting a child with no active transaction should fail
    sentry_span_t *parentless_child
        = sentry_transaction_start_child(NULL, NULL, NULL);
    TEST_CHECK(!parentless_child);

    sentry_transaction_context_t *opaque_tx_ctx
        = sentry_transaction_context_new("wow!", NULL);
    sentry_transaction_t *opaque_tx
        = sentry_transaction_start(opaque_tx_ctx, sentry_value_new_null());
    sentry_value_t tx = opaque_tx->inner;

    sentry_span_t *opaque_child
        = sentry_transaction_start_child(opaque_tx, "honk", "goose");
    sentry_value_t child = opaque_child->inner;
    TEST_CHECK(!sentry_value_is_null(child));

    // Peek into the transaction's span list and make sure everything is
    // good
    const char *trace_id
        = sentry_value_as_string(sentry_value_get_by_key(tx, "trace_id"));
    const char *parent_span_id
        = sentry_value_as_string(sentry_value_get_by_key(tx, "span_id"));
    // Don't track the span yet
    TEST_CHECK(IS_NULL(tx, "spans"));

    // Sanity check that child isn't finished yet
    TEST_CHECK(IS_NULL(child, "timestamp"));
    // Now finishing
    sentry_span_finish(opaque_child);

    TEST_CHECK(!IS_NULL(tx, "spans"));
    sentry_value_t spans = sentry_value_get_by_key(tx, "spans");
    TEST_CHECK_INT_EQUAL(sentry_value_get_length(spans), 1);

    sentry_value_t stored_child = sentry_value_get_by_index(spans, 0);
    // Make sure the span inherited everything correctly
    CHECK_STRING_PROPERTY(stored_child, "trace_id", trace_id);
    CHECK_STRING_PROPERTY(stored_child, "parent_span_id", parent_span_id);
    CHECK_STRING_PROPERTY(stored_child, "op", "honk");
    CHECK_STRING_PROPERTY(stored_child, "description", "goose");
    // Should be finished
    TEST_CHECK(!IS_NULL(stored_child, "timestamp"));

    sentry__transaction_decref(opaque_tx);

    sentry_close();
}

SENTRY_TEST(spans_on_scope)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_init(options);

    sentry_transaction_context_t *opaque_tx_ctx
        = sentry_transaction_context_new("wow!", NULL);
    sentry_transaction_t *opaque_tx
        = sentry_transaction_start(opaque_tx_ctx, sentry_value_new_null());
    sentry_set_transaction_object(opaque_tx);

    sentry_span_t *opaque_child
        = sentry_transaction_start_child(opaque_tx, "honk", "goose");
    sentry_value_t child = opaque_child->inner;
    TEST_CHECK(!sentry_value_is_null(child));

    // Peek into the transaction's span list and make sure everything is
    // good
    sentry_value_t scope_tx = sentry__scope_get_span_or_transaction();
    const char *trace_id
        = sentry_value_as_string(sentry_value_get_by_key(scope_tx, "trace_id"));
    const char *parent_span_id
        = sentry_value_as_string(sentry_value_get_by_key(scope_tx, "span_id"));
    // Don't track the span yet
    TEST_CHECK(IS_NULL(scope_tx, "spans"));

    // Sanity check that child isn't finished yet
    TEST_CHECK(IS_NULL(child, "timestamp"));

    sentry_span_finish(opaque_child);

    scope_tx = sentry__scope_get_span_or_transaction();
    TEST_CHECK(!IS_NULL(scope_tx, "spans"));
    sentry_value_t spans = sentry_value_get_by_key(scope_tx, "spans");
    TEST_CHECK_INT_EQUAL(sentry_value_get_length(spans), 1);

    sentry_value_t stored_child = sentry_value_get_by_index(spans, 0);
    // Make sure the span inherited everything correctly
    CHECK_STRING_PROPERTY(stored_child, "trace_id", trace_id);
    CHECK_STRING_PROPERTY(stored_child, "parent_span_id", parent_span_id);
    CHECK_STRING_PROPERTY(stored_child, "op", "honk");
    CHECK_STRING_PROPERTY(stored_child, "description", "goose");
    // Should be finished
    TEST_CHECK(!IS_NULL(stored_child, "timestamp"));

    sentry__transaction_decref(opaque_tx);

    sentry_close();
}

void
run_child_spans_test(bool timestamped)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_options_set_max_spans(options, 3);
    sentry_init(options);

    sentry_transaction_context_t *opaque_tx_ctx
        = sentry_transaction_context_new("wow!", NULL);
    sentry_transaction_t *opaque_tx;
    if (timestamped) {
        opaque_tx = sentry_transaction_start_ts(
            opaque_tx_ctx, sentry_value_new_null(), 1);
        CHECK_STRING_PROPERTY(
            opaque_tx->inner, "start_timestamp", "1970-01-01T00:00:00.000001Z");
    } else {
        opaque_tx
            = sentry_transaction_start(opaque_tx_ctx, sentry_value_new_null());
    }
    sentry_value_t tx = opaque_tx->inner;

    sentry_span_t *opaque_child;
    if (timestamped) {
        opaque_child
            = sentry_transaction_start_child_ts(opaque_tx, "honk", "goose", 2);
        CHECK_STRING_PROPERTY(opaque_child->inner, "start_timestamp",
            "1970-01-01T00:00:00.000002Z");
    } else {
        opaque_child
            = sentry_transaction_start_child(opaque_tx, "honk", "goose");
    }
    sentry_value_t child = opaque_child->inner;
    TEST_CHECK(!sentry_value_is_null(child));
    // Shouldn't be added to spans yet
    TEST_CHECK(IS_NULL(tx, "spans"));

    sentry_span_t *opaque_grandchild;
    if (timestamped) {
        opaque_grandchild
            = sentry_span_start_child_ts(opaque_child, "beep", "car", 3);
        CHECK_STRING_PROPERTY(opaque_grandchild->inner, "start_timestamp",
            "1970-01-01T00:00:00.000003Z");
    } else {
        opaque_grandchild
            = sentry_span_start_child(opaque_child, "beep", "car");
    }
    sentry_value_t grandchild = opaque_grandchild->inner;
    TEST_CHECK(!sentry_value_is_null(grandchild));
    // Shouldn't be added to spans yet
    TEST_CHECK(IS_NULL(tx, "spans"));

    if (timestamped) {
        sentry_span_finish_ts(opaque_grandchild, 4);
    } else {
        sentry_span_finish(opaque_grandchild);
    }

    // Make sure everything on the transaction looks good, check grandchild
    const char *trace_id
        = sentry_value_as_string(sentry_value_get_by_key(tx, "trace_id"));
    const char *parent_span_id
        = sentry_value_as_string(sentry_value_get_by_key(child, "span_id"));

    TEST_CHECK(!IS_NULL(tx, "spans"));
    sentry_value_t spans = sentry_value_get_by_key(tx, "spans");
    TEST_CHECK_INT_EQUAL(sentry_value_get_length(spans), 1);

    sentry_value_t stored_grandchild = sentry_value_get_by_index(spans, 0);
    CHECK_STRING_PROPERTY(stored_grandchild, "trace_id", trace_id);
    CHECK_STRING_PROPERTY(stored_grandchild, "parent_span_id", parent_span_id);
    CHECK_STRING_PROPERTY(stored_grandchild, "op", "beep");
    CHECK_STRING_PROPERTY(stored_grandchild, "description", "car");
    // Should be finished
    TEST_CHECK(!IS_NULL(stored_grandchild, "timestamp"));

    if (timestamped) {
        sentry_span_finish_ts(opaque_child, 5);
    } else {
        sentry_span_finish(opaque_child);
    }
    spans = sentry_value_get_by_key(tx, "spans");
    TEST_CHECK_INT_EQUAL(sentry_value_get_length(spans), 2);

    sentry__transaction_decref(opaque_tx);
    sentry_close();
}

SENTRY_TEST(child_spans) { run_child_spans_test(false); }

SENTRY_TEST(child_spans_ts) { run_child_spans_test(true); }

SENTRY_TEST(overflow_spans)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_options_set_max_spans(options, 1);
    sentry_init(options);

    sentry_transaction_context_t *opaque_tx_ctx
        = sentry_transaction_context_new("wow!", NULL);
    sentry_transaction_t *opaque_tx
        = sentry_transaction_start(opaque_tx_ctx, sentry_value_new_null());
    sentry_value_t tx = opaque_tx->inner;

    sentry_span_t *opaque_child
        = sentry_transaction_start_child(opaque_tx, "honk", "goose");
    sentry_value_t child = opaque_child->inner;
    const char *child_span_id
        = sentry_value_as_string(sentry_value_get_by_key(child, "span_id"));

    // Shouldn't be added to spans yet
    TEST_CHECK(IS_NULL(tx, "spans"));

    sentry_span_t *opaque_drop_on_finish_child
        = sentry_span_start_child(opaque_child, "beep", "car");
    sentry_value_t drop_on_finish_child = opaque_drop_on_finish_child->inner;
    TEST_CHECK(!sentry_value_is_null(drop_on_finish_child));
    // Shouldn't be added to spans yet
    TEST_CHECK(IS_NULL(tx, "spans"));

    sentry_span_finish(opaque_child);

    TEST_CHECK(!IS_NULL(tx, "spans"));
    sentry_value_t spans = sentry_value_get_by_key(tx, "spans");
    TEST_CHECK_INT_EQUAL(sentry_value_get_length(spans), 1);

    sentry_value_t stored_child = sentry_value_get_by_index(spans, 0);
    CHECK_STRING_PROPERTY(stored_child, "span_id", child_span_id);

    sentry_span_finish(opaque_drop_on_finish_child);
    TEST_CHECK_INT_EQUAL(sentry_value_get_length(spans), 1);

    sentry_span_t *opaque_drop_on_start_child
        = sentry_transaction_start_child(opaque_tx, "ring", "bicycle");
    TEST_CHECK(!opaque_drop_on_start_child);
    TEST_CHECK_INT_EQUAL(sentry_value_get_length(spans), 1);

    sentry__transaction_decref(opaque_tx);

    sentry_close();
}

SENTRY_TEST(unsampled_spans)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_init(options);

    sentry_transaction_context_t *opaque_tx_ctx
        = sentry_transaction_context_new("noisemakers", NULL);
    sentry_transaction_context_set_sampled(opaque_tx_ctx, 0);
    sentry_transaction_t *opaque_tx
        = sentry_transaction_start(opaque_tx_ctx, sentry_value_new_null());
    sentry_value_t tx = opaque_tx->inner;
    TEST_CHECK(!sentry_value_is_true(sentry_value_get_by_key(tx, "sampled")));

    // check that children and grandchildren inherit the sampling decision,
    // i.e. it cascades 1+ levels down
    sentry_span_t *opaque_child
        = sentry_transaction_start_child(opaque_tx, "honk", "goose");
    sentry_value_t child = opaque_child->inner;
    TEST_CHECK(!sentry_value_is_null(child));
    TEST_CHECK(
        !sentry_value_is_true(sentry_value_get_by_key(child, "sampled")));

    sentry_span_t *opaque_grandchild
        = sentry_span_start_child(opaque_child, "beep", "car");
    sentry_value_t grandchild = opaque_grandchild->inner;
    TEST_CHECK(!sentry_value_is_null(grandchild));
    TEST_CHECK(
        !sentry_value_is_true(sentry_value_get_by_key(grandchild, "sampled")));

    // finishing does not add (grand)children to the spans list
    sentry_span_finish(opaque_grandchild);
    TEST_CHECK(
        0 == sentry_value_get_length(sentry_value_get_by_key(tx, "spans")));

    sentry_span_finish(opaque_child);
    TEST_CHECK(
        0 == sentry_value_get_length(sentry_value_get_by_key(tx, "spans")));

    // perform the same checks, but with the transaction on the scope
    sentry_set_transaction_object(opaque_tx);

    opaque_child = sentry_transaction_start_child(opaque_tx, "toot", "boat");
    child = opaque_child->inner;
    TEST_CHECK(!sentry_value_is_null(child));
    TEST_CHECK(
        !sentry_value_is_true(sentry_value_get_by_key(child, "sampled")));

    opaque_grandchild
        = sentry_span_start_child(opaque_child, "vroom", "sportscar");
    grandchild = opaque_grandchild->inner;
    TEST_CHECK(!sentry_value_is_null(grandchild));
    TEST_CHECK(
        !sentry_value_is_true(sentry_value_get_by_key(grandchild, "sampled")));

    sentry_span_finish(opaque_grandchild);
    TEST_CHECK(
        0 == sentry_value_get_length(sentry_value_get_by_key(tx, "spans")));

    sentry_span_finish(opaque_child);
    TEST_CHECK(
        0 == sentry_value_get_length(sentry_value_get_by_key(tx, "spans")));

    sentry_transaction_finish(opaque_tx);

    sentry_close();
}

static void
check_spans(sentry_envelope_t *envelope, void *data)
{
    uint64_t *called = data;
    *called += 1;

    sentry_value_t transaction = sentry_envelope_get_transaction(envelope);
    TEST_CHECK(!sentry_value_is_null(transaction));

    size_t span_count = sentry_value_get_length(
        sentry_value_get_by_key(transaction, "spans"));
    TEST_CHECK_INT_EQUAL(span_count, 1);

    sentry_envelope_free(envelope);
}

SENTRY_TEST(drop_unfinished_spans)
{
    uint64_t called_transport = 0;

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    // Disable sessions or this test would fail if env:SENTRY_RELEASE is set.
    sentry_options_set_auto_session_tracking(options, 0);

    sentry_transport_t *transport = sentry_transport_new(check_spans);
    sentry_transport_set_state(transport, &called_transport);
    sentry_options_set_transport(options, transport);

    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_options_set_max_spans(options, 2);
    sentry_init(options);

    sentry_transaction_context_t *opaque_tx_ctx
        = sentry_transaction_context_new("wow!", NULL);
    sentry_transaction_t *opaque_tx
        = sentry_transaction_start(opaque_tx_ctx, sentry_value_new_null());
    sentry_value_t tx = opaque_tx->inner;

    sentry_span_t *opaque_child
        = sentry_transaction_start_child(opaque_tx, "honk", "goose");
    sentry_value_t child = opaque_child->inner;
    TEST_CHECK(!sentry_value_is_null(child));

    sentry_span_t *opaque_grandchild
        = sentry_span_start_child(opaque_child, "beep", "car");
    sentry_value_t grandchild = opaque_grandchild->inner;
    TEST_CHECK(!sentry_value_is_null(grandchild));
    sentry_span_finish(opaque_grandchild);

    // spans are only added to transactions upon completion
    TEST_CHECK_INT_EQUAL(
        sentry_value_get_length(sentry_value_get_by_key(tx, "spans")), 1);

    sentry_uuid_t event_id = sentry_transaction_finish(opaque_tx);
    TEST_CHECK(!sentry_uuid_is_nil(&event_id));

    // check that nothing explodes if you do finish the lingering child
    sentry_span_finish(opaque_child);

    sentry_close();

    TEST_CHECK_INT_EQUAL(called_transport, 1);
}

static void
forward_headers_to(const char *key, const char *value, void *userdata)
{
    sentry_transaction_context_t *tx_ctx
        = (sentry_transaction_context_t *)userdata;

    sentry_transaction_context_update_from_header(tx_ctx, key, value);
}

SENTRY_TEST(update_from_header_null_ctx)
{
    sentry_transaction_context_update_from_header(
        NULL, "irrelevant-key", "irrelevant-value");
}

SENTRY_TEST(update_from_header_no_sampled_flag)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");

    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_options_set_max_spans(options, 2);
    sentry_init(options);

    sentry_transaction_context_update_from_header(
        NULL, "irrelevant-key", "irrelevant-value");
    const char *trace_header
        = "2674eb52d5874b13b560236d6c79ce8a-a0f9fdf04f1a63df";
    sentry_transaction_context_t *tx_ctx
        = sentry_transaction_context_new("wow!", NULL);
    sentry_transaction_context_update_from_header(
        tx_ctx, "sentry-trace", trace_header);
    sentry_transaction_t *tx
        = sentry_transaction_start(tx_ctx, sentry_value_new_null());

    CHECK_STRING_PROPERTY(
        tx->inner, "trace_id", "2674eb52d5874b13b560236d6c79ce8a");
    CHECK_STRING_PROPERTY(tx->inner, "parent_span_id", "a0f9fdf04f1a63df");
    sentry_value_t sampled = sentry_value_get_by_key(tx->inner, "sampled");
    TEST_CHECK(sentry_value_get_type(sampled) == SENTRY_VALUE_TYPE_BOOL);
    TEST_CHECK(sentry_value_is_true(sampled));

    sentry__transaction_decref(tx);
    sentry_close();
}

SENTRY_TEST(distributed_headers_invalid_traceid)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");

    sentry_init(options);

    sentry_transaction_context_t *tx_ctx
        = sentry_transaction_context_new("sanity_check", NULL);

    const char *valid_trace_header
        = "2674eb52d5874b13b560236d6c79ce8a-a0f9fdf04f1a63df-1";
    // expected should match the valid trace_id from the header
    const char *expected_trace_id = "2674eb52d5874b13b560236d6c79ce8a";

    // sanity check test case
    sentry_transaction_context_update_from_header(
        tx_ctx, "sentry-trace", valid_trace_header);
    const char *valid_trace_id = sentry_value_as_string(
        sentry_value_get_by_key(tx_ctx->inner, "trace_id"));
    TEST_CHECK_STRING_EQUAL(valid_trace_id, expected_trace_id);

    // case 1: string with two dashes (nothing inbetween)
    const char *trace_header = "--";
    sentry_transaction_context_update_from_header(
        tx_ctx, "sentry-trace", trace_header);
    const char *new_trace_id = sentry_value_as_string(
        sentry_value_get_by_key(tx_ctx->inner, "trace_id"));
    // expect to have the trace_id remain unchanged
    TEST_CHECK_STRING_EQUAL(new_trace_id, expected_trace_id);

    // case 2: string with two dashes (trace_id too short)
    const char *trace_header_short = "2-a0f9fdf04f1a63df-1";
    sentry_transaction_context_update_from_header(
        tx_ctx, "sentry-trace", trace_header_short);
    const char *new_trace_id_short = sentry_value_as_string(
        sentry_value_get_by_key(tx_ctx->inner, "trace_id"));
    // expect to have the trace_id remain unchanged
    TEST_CHECK_STRING_EQUAL(new_trace_id_short, expected_trace_id);

    // case 3: string with two dashes (trace_id too long)
    const char *trace_header_long = "2674eb52d5874b13b560236d6c79ce8a2674eb52d5"
                                    "874b13b560236d6c79ce8a-a0f9fdf04f1a63df-1";
    sentry_transaction_context_update_from_header(
        tx_ctx, "sentry-trace", trace_header_long);
    const char *new_trace_id_long = sentry_value_as_string(
        sentry_value_get_by_key(tx_ctx->inner, "trace_id"));
    // expect to have the trace_id remain unchanged
    TEST_CHECK_STRING_EQUAL(new_trace_id_long, expected_trace_id);

    sentry__transaction_context_free(tx_ctx);
    sentry_close();
}

SENTRY_TEST(distributed_headers_invalid_spanid)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");

    sentry_init(options);

    sentry_transaction_context_t *tx_ctx
        = sentry_transaction_context_new("wow!", NULL);

    const char *valid_trace_header
        = "2674eb52d5874b13b560236d6c79ce8a-a0f9fdf04f1a63df-1";
    // expected should match the valid parent_span_id from the header
    const char *expected_parent_span_id = "a0f9fdf04f1a63df";

    // sanity check test case
    sentry_transaction_context_update_from_header(
        tx_ctx, "sentry-trace", valid_trace_header);
    const char *valid_parent_span_id = sentry_value_as_string(
        sentry_value_get_by_key(tx_ctx->inner, "parent_span_id"));
    TEST_CHECK_STRING_EQUAL(valid_parent_span_id, expected_parent_span_id);

    // case 1: string with two dashes (nothing inbetween)
    const char *trace_header = "--";
    sentry_transaction_context_update_from_header(
        tx_ctx, "sentry-trace", trace_header);
    const char *new_parent_span_id = sentry_value_as_string(
        sentry_value_get_by_key(tx_ctx->inner, "parent_span_id"));
    // expect to have the parent_span_id remain unchanged
    TEST_CHECK_STRING_EQUAL(new_parent_span_id, expected_parent_span_id);

    // case 2: string with two dashes (parent_span_id too short)
    const char *trace_header_short = "2674eb52d5874b13b560236d6c79ce8a-a-1";
    sentry_transaction_context_update_from_header(
        tx_ctx, "sentry-trace", trace_header_short);
    const char *new_parent_span_id_short = sentry_value_as_string(
        sentry_value_get_by_key(tx_ctx->inner, "parent_span_id"));
    // expect to have the parent_span_id remain unchanged
    TEST_CHECK_STRING_EQUAL(new_parent_span_id_short, expected_parent_span_id);

    // case 3: string with two dashes (parent_span_id too long)
    const char *trace_header_long
        = "2674eb52d5874b13b560236d6c79ce8a-a0f9fdf04f1a63dfa0f9fdf04f1a63df-1";
    sentry_transaction_context_update_from_header(
        tx_ctx, "sentry-trace", trace_header_long);
    const char *new_parent_span_id_long = sentry_value_as_string(
        sentry_value_get_by_key(tx_ctx->inner, "parent_span_id"));
    // expect to have the parent_span_id remain unchanged
    TEST_CHECK_STRING_EQUAL(new_parent_span_id_long, expected_parent_span_id);

    // case 4: string with one dash (span_id empty)
    const char *trace_header_empty_span = "2674eb52d5874b13b560236d6c79ce8a-";
    sentry_transaction_context_update_from_header(
        tx_ctx, "sentry-trace", trace_header_empty_span);
    const char *new_parent_span_id_empty = sentry_value_as_string(
        sentry_value_get_by_key(tx_ctx->inner, "parent_span_id"));
    // expect to have the parent_span_id remain unchanged
    TEST_CHECK_STRING_EQUAL(new_parent_span_id_empty, expected_parent_span_id);

    sentry__transaction_context_free(tx_ctx);
    sentry_close();
}

SENTRY_TEST(distributed_headers)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");

    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_options_set_max_spans(options, 2);
    sentry_init(options);

    const char *trace_header
        = "2674eb52d5874b13b560236d6c79ce8a-a0f9fdf04f1a63df-1";
    const char *not_expected_header
        = "00000000000000000000000000000000-0000000000000000-1";
    const char *expected_trace_id = "2674eb52d5874b13b560236d6c79ce8a";

    sentry_transaction_context_t *tx_ctx
        = sentry_transaction_context_new("wow!", NULL);

    // check case-insensitive headers, and bogus header names
    sentry_transaction_context_update_from_header(
        tx_ctx, "SeNtry-TrAcE", trace_header);
    sentry_transaction_context_update_from_header(
        tx_ctx, "sentry_trace", not_expected_header);
    sentry_transaction_context_update_from_header(
        tx_ctx, NULL, not_expected_header);
    sentry_transaction_context_update_from_header(tx_ctx, "sentry-trace", NULL);
    sentry_transaction_context_update_from_header(
        tx_ctx, "nop", not_expected_header);
    sentry_transaction_context_update_from_header(
        tx_ctx, "sentry-trace-but-a-lot-longer", not_expected_header);

    sentry_transaction_t *tx
        = sentry_transaction_start(tx_ctx, sentry_value_new_null());

    const char *trace_id = sentry_value_as_string(
        sentry_value_get_by_key(tx->inner, "trace_id"));
    TEST_CHECK_STRING_EQUAL(trace_id, expected_trace_id);

    const char *span_id
        = sentry_value_as_string(sentry_value_get_by_key(tx->inner, "span_id"));
    TEST_CHECK(!sentry__string_eq(span_id, ""));

    // check transaction
    tx_ctx = sentry_transaction_context_new("distributed!", NULL);
    sentry_transaction_iter_headers(tx, forward_headers_to, (void *)tx_ctx);
    sentry_transaction_t *dist_tx
        = sentry_transaction_start(tx_ctx, sentry_value_new_null());

    const char *dist_trace_id = sentry_value_as_string(
        sentry_value_get_by_key(dist_tx->inner, "trace_id"));
    TEST_CHECK_STRING_EQUAL(dist_trace_id, trace_id);

    const char *parent_span_id = sentry_value_as_string(
        sentry_value_get_by_key(dist_tx->inner, "parent_span_id"));
    TEST_CHECK_STRING_EQUAL(parent_span_id, span_id);

    sentry__transaction_decref(dist_tx);

    // check span
    sentry_span_t *child = sentry_transaction_start_child(tx, "honk", "goose");

    span_id = sentry_value_as_string(
        sentry_value_get_by_key(child->inner, "span_id"));
    TEST_CHECK(!sentry__string_eq(span_id, ""));

    tx_ctx = sentry_transaction_context_new("distributed!", NULL);
    sentry_span_iter_headers(child, forward_headers_to, (void *)tx_ctx);
    dist_tx = sentry_transaction_start(tx_ctx, sentry_value_new_null());

    dist_trace_id = sentry_value_as_string(
        sentry_value_get_by_key(dist_tx->inner, "trace_id"));
    TEST_CHECK_STRING_EQUAL(dist_trace_id, trace_id);

    parent_span_id = sentry_value_as_string(
        sentry_value_get_by_key(dist_tx->inner, "parent_span_id"));
    TEST_CHECK_STRING_EQUAL(parent_span_id, span_id);

    TEST_CHECK(sentry_value_is_true(
        sentry_value_get_by_key(dist_tx->inner, "sampled")));

    sentry__transaction_decref(dist_tx);
    sentry__span_decref(child);
    sentry__transaction_decref(tx);

    // check sampled flag
    tx_ctx = sentry_transaction_context_new("wow!", NULL);
    sentry_transaction_context_set_sampled(tx_ctx, 0);
    tx = sentry_transaction_start(tx_ctx, sentry_value_new_null());

    tx_ctx = sentry_transaction_context_new("distributed!", NULL);
    sentry_transaction_iter_headers(tx, forward_headers_to, (void *)tx_ctx);
    dist_tx = sentry_transaction_start(tx_ctx, sentry_value_new_null());

    TEST_CHECK(!sentry_value_is_true(
        sentry_value_get_by_key(dist_tx->inner, "sampled")));

    child = sentry_transaction_start_child(tx, "honk", "goose");
    TEST_CHECK(!sentry_value_is_true(
        sentry_value_get_by_key(child->inner, "sampled")));

    tx_ctx = sentry_transaction_context_new("distributed from a child!", NULL);
    sentry_span_iter_headers(child, forward_headers_to, (void *)tx_ctx);
    sentry__transaction_decref(dist_tx);
    dist_tx = sentry_transaction_start(tx_ctx, sentry_value_new_null());

    TEST_CHECK(!sentry_value_is_true(
        sentry_value_get_by_key(dist_tx->inner, "sampled")));

    sentry__transaction_decref(dist_tx);
    sentry__span_decref(child);
    sentry__transaction_decref(tx);

    sentry_close();
}

void
check_after_set(sentry_value_t inner, const char *inner_key,
    const char *item_key, const char *expected)
{
    sentry_value_t inner_tags = sentry_value_get_by_key(inner, inner_key);
    TEST_CHECK_INT_EQUAL(1, sentry_value_get_length(inner_tags));
    TEST_CHECK(
        sentry_value_get_type(sentry_value_get_by_key(inner_tags, item_key))
        == SENTRY_VALUE_TYPE_STRING);
    CHECK_STRING_PROPERTY(inner_tags, item_key, expected);
}

void
check_after_remove(
    sentry_value_t inner, const char *inner_key, const char *item_key)
{
    sentry_value_t inner_tags = sentry_value_get_by_key(inner, inner_key);
    TEST_CHECK_INT_EQUAL(0, sentry_value_get_length(inner_tags));
    TEST_CHECK(IS_NULL(inner_tags, item_key));
}

SENTRY_TEST(txn_tagging)
{
    sentry_transaction_t *txn
        = sentry__transaction_new(sentry_value_new_object());
    TEST_ASSERT(!!txn);

    sentry_transaction_set_tag(txn, "os.name", "Linux");
    check_after_set(txn->inner, "tags", "os.name", "Linux");

    sentry_transaction_remove_tag(txn, "os.name");
    check_after_remove(txn->inner, "tags", "os.name");

    sentry__transaction_decref(txn);
}

SENTRY_TEST(span_tagging)
{
    sentry_transaction_t *txn
        = sentry__transaction_new(sentry_value_new_object());
    TEST_ASSERT(!!txn);
    sentry_span_t *span = sentry__span_new(txn, sentry_value_new_object());
    TEST_ASSERT(!!span);

    sentry_span_set_tag(span, "os.name", "Linux");
    check_after_set(span->inner, "tags", "os.name", "Linux");

    sentry_span_remove_tag(span, "os.name");
    check_after_remove(span->inner, "tags", "os.name");

    sentry__span_decref(span);
    sentry__transaction_decref(txn);
}

SENTRY_TEST(txn_tagging_n)
{
    sentry_transaction_t *txn
        = sentry__transaction_new(sentry_value_new_object());
    TEST_ASSERT(!!txn);

    char tag[] = { 'o', 's', '.', 'n', 'a', 'm', 'e' };
    char tag_val[] = { 'L', 'i', 'n', 'u', 'x' };
    sentry_transaction_set_tag_n(
        txn, tag, sizeof(tag), tag_val, sizeof(tag_val));
    check_after_set(txn->inner, "tags", "os.name", "Linux");

    sentry_transaction_remove_tag_n(txn, tag, sizeof(tag));
    check_after_remove(txn->inner, "tags", "os.name");

    sentry__transaction_decref(txn);
}

SENTRY_TEST(span_tagging_n)
{
    sentry_transaction_t *txn
        = sentry__transaction_new(sentry_value_new_object());
    TEST_ASSERT(!!txn);
    sentry_span_t *span = sentry__span_new(txn, sentry_value_new_object());
    TEST_ASSERT(!!span);

    char tag[] = { 'o', 's', '.', 'n', 'a', 'm', 'e' };
    char tag_val[] = { 'L', 'i', 'n', 'u', 'x' };
    sentry_span_set_tag_n(span, tag, sizeof(tag), tag_val, sizeof(tag_val));
    check_after_set(span->inner, "tags", "os.name", "Linux");

    sentry_span_remove_tag_n(span, tag, sizeof(tag));
    check_after_remove(span->inner, "tags", "os.name");

    sentry__span_decref(span);
    sentry__transaction_decref(txn);
}

SENTRY_TEST(txn_name)
{
    sentry_transaction_t *txn
        = sentry__transaction_new(sentry_value_new_object());
    TEST_ASSERT(!!txn);

    char *txn_name = "the_txn";
    sentry_transaction_set_name(txn, txn_name);
    sentry_value_t txn_name_value
        = sentry_value_get_by_key(txn->inner, "transaction");
    TEST_CHECK(
        sentry_value_get_type(txn_name_value) == SENTRY_VALUE_TYPE_STRING);
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(txn_name_value), txn_name);

    sentry__transaction_decref(txn);
}

SENTRY_TEST(txn_data)
{
    sentry_transaction_t *txn
        = sentry__transaction_new(sentry_value_new_object());
    TEST_ASSERT(!!txn);

    sentry_transaction_set_data(
        txn, "os.name", sentry_value_new_string("Linux"));
    check_after_set(txn->inner, "data", "os.name", "Linux");

    sentry_transaction_remove_data(txn, "os.name");
    check_after_remove(txn->inner, "data", "os.name");

    sentry__transaction_decref(txn);
}

SENTRY_TEST(span_data)
{
    sentry_transaction_t *txn
        = sentry__transaction_new(sentry_value_new_object());
    TEST_ASSERT(!!txn);
    sentry_span_t *span = sentry__span_new(txn, sentry_value_new_object());
    TEST_ASSERT(!!span);

    sentry_span_set_data(span, "os.name", sentry_value_new_string("Linux"));
    check_after_set(span->inner, "data", "os.name", "Linux");

    sentry_span_remove_data(span, "os.name");
    check_after_remove(span->inner, "data", "os.name");

    sentry__span_decref(span);
    sentry__transaction_decref(txn);
}

SENTRY_TEST(txn_name_n)
{
    sentry_transaction_t *txn
        = sentry__transaction_new(sentry_value_new_object());
    TEST_ASSERT(!!txn);
    char txn_name[] = { 't', 'h', 'e', '_', 't', 'x', 'n' };
    sentry_transaction_set_name_n(txn, txn_name, sizeof(txn_name));

    sentry_value_t txn_name_value
        = sentry_value_get_by_key(txn->inner, "transaction");
    TEST_CHECK(
        sentry_value_get_type(txn_name_value) == SENTRY_VALUE_TYPE_STRING);
    const char *txn_name_str = sentry_value_as_string(txn_name_value);
    TEST_ASSERT(!!txn_name_str);
    TEST_CHECK_STRING_EQUAL(txn_name_str, "the_txn");

    sentry__transaction_decref(txn);
}

SENTRY_TEST(txn_data_n)
{
    sentry_transaction_t *txn
        = sentry__transaction_new(sentry_value_new_object());
    TEST_ASSERT(!!txn);

    char data_k[] = { 'o', 's', '.', 'n', 'a', 'm', 'e' };
    char data_v[] = { 'L', 'i', 'n', 'u', 'x' };
    sentry_value_t data_value
        = sentry_value_new_string_n(data_v, sizeof(data_v));
    sentry_transaction_set_data_n(txn, data_k, sizeof(data_k), data_value);
    check_after_set(txn->inner, "data", "os.name", "Linux");

    sentry_transaction_remove_data_n(txn, data_k, sizeof(data_k));
    check_after_remove(txn->inner, "data", "os.name");

    sentry__transaction_decref(txn);
}

SENTRY_TEST(span_data_n)
{
    sentry_transaction_t *txn
        = sentry__transaction_new(sentry_value_new_object());
    TEST_ASSERT(!!txn);
    sentry_span_t *span = sentry__span_new(txn, sentry_value_new_object());
    TEST_ASSERT(!!span);

    char data_k[] = { 'o', 's', '.', 'n', 'a', 'm', 'e' };
    char data_v[] = { 'L', 'i', 'n', 'u', 'x' };
    sentry_value_t data_value
        = sentry_value_new_string_n(data_v, sizeof(data_v));
    sentry_span_set_data_n(span, data_k, sizeof(data_k), data_value);
    check_after_set(span->inner, "data", "os.name", "Linux");

    sentry_span_remove_data_n(span, data_k, sizeof(data_k));
    check_after_remove(span->inner, "data", "os.name");

    sentry__span_decref(span);
    sentry__transaction_decref(txn);
}

SENTRY_TEST(sentry__value_span_new_requires_unfinished_parent)
{
    sentry_value_t parent = sentry_value_new_object();
    // timestamps are typically iso8601 strings, but this is irrelevant to
    // `sentry__value_span_new` which just wants `timestamp` to not be null.
    sentry_value_set_by_key(parent, "timestamp", sentry_value_new_object());
    sentry_value_t inner_span
        = sentry__value_span_new(0, parent, NULL, NULL, 0);
    TEST_CHECK(sentry_value_is_null(inner_span));

    sentry_value_decref(parent);
}

SENTRY_TEST(set_tag_allows_null_tag_and_value)
{
    sentry_transaction_t *txn
        = sentry__transaction_new(sentry_value_new_object());
    TEST_ASSERT(!!txn);
    sentry_transaction_set_tag(txn, NULL, NULL);
    sentry_value_t tags = sentry_value_get_by_key(txn->inner, "tags");
    TEST_CHECK(!sentry_value_is_null(tags));
    TEST_CHECK(sentry_value_get_type(tags) == SENTRY_VALUE_TYPE_OBJECT);
    TEST_CHECK(sentry_value_get_length(tags) == 0);

    sentry_transaction_set_tag(txn, "os.name", NULL);
    tags = sentry_value_get_by_key(txn->inner, "tags");
    TEST_CHECK(!sentry_value_is_null(tags));
    TEST_CHECK(sentry_value_get_type(tags) == SENTRY_VALUE_TYPE_OBJECT);
    TEST_CHECK(sentry_value_get_length(tags) == 1);
    TEST_CHECK(IS_NULL(tags, "os.name"));

    sentry__transaction_decref(txn);
}

SENTRY_TEST(set_tag_cuts_value_at_length_200)
{
    const char test_value[]
        = "012345678901234567890123456789012345678901234567890123456789"
          "012345678901234567890123456789012345678901234567890123456789"
          "012345678901234567890123456789012345678901234567890123456789"
          "012345678901234567890123456789012345678901234567890123456789";

    sentry_transaction_t *txn
        = sentry__transaction_new(sentry_value_new_object());
    TEST_ASSERT(!!txn);
    sentry_transaction_set_tag(txn, "cut-off", test_value);
    sentry_value_t tags = sentry_value_get_by_key(txn->inner, "tags");
    TEST_CHECK(!sentry_value_is_null(tags));
    TEST_CHECK(sentry_value_get_type(tags) == SENTRY_VALUE_TYPE_OBJECT);
    TEST_CHECK(sentry_value_get_length(tags) == 1);
    const char *cut_off
        = sentry_value_as_string(sentry_value_get_by_key(tags, "cut-off"));
    TEST_ASSERT(!!cut_off);
    TEST_CHECK_INT_EQUAL(strlen(cut_off), 200);

    sentry__transaction_decref(txn);
}

SENTRY_TEST(set_trace)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_init(options);

    const char *trace_id = "2674eb52d5874b13b560236d6c79ce8a";
    const char *parent_span_id = "a0f9fdf04f1a63df";

    sentry_set_trace(trace_id, parent_span_id);

    SENTRY_WITH_SCOPE (scope) {
        sentry_value_t propagation_trace_context
            = sentry_value_get_by_key(scope->propagation_context, "trace");
        TEST_CHECK(!sentry_value_is_null(propagation_trace_context));

        CHECK_STRING_PROPERTY(propagation_trace_context, "type", "trace");

        CHECK_STRING_PROPERTY(propagation_trace_context, "trace_id", trace_id);
        CHECK_STRING_PROPERTY(
            propagation_trace_context, "parent_span_id", parent_span_id);

        const char *span_id = sentry_value_as_string(
            sentry_value_get_by_key(propagation_trace_context, "span_id"));
        TEST_ASSERT(!!span_id);
        TEST_CHECK(strlen(span_id) > 0);
    }

    sentry_close();
}

void
apply_scope_and_check_trace_context(
    sentry_options_t *options, const char *trace_id, const char *parent_span_id)
{
    // simulate scope application onto an event
    sentry_value_t event = sentry_value_new_object();
    SENTRY_WITH_SCOPE (scope) {
        sentry__scope_apply_to_event(scope, options, event, SENTRY_SCOPE_NONE);
    }

    // check that the event has a trace context
    sentry_value_t event_contexts = sentry_value_get_by_key(event, "contexts");
    TEST_CHECK(!sentry_value_is_null(event_contexts));
    TEST_CHECK(
        sentry_value_get_type(event_contexts) == SENTRY_VALUE_TYPE_OBJECT);

    sentry_value_t event_trace_context
        = sentry_value_get_by_key(event_contexts, "trace");
    TEST_CHECK(!sentry_value_is_null(event_trace_context));
    TEST_CHECK(
        sentry_value_get_type(event_trace_context) == SENTRY_VALUE_TYPE_OBJECT);

    // check trace context content
    const char *event_trace_id = sentry_value_as_string(
        sentry_value_get_by_key(event_trace_context, "trace_id"));
    TEST_ASSERT(!!event_trace_id);
    TEST_CHECK_STRING_EQUAL(event_trace_id, trace_id);

    const char *event_trace_parent_span_id = sentry_value_as_string(
        sentry_value_get_by_key(event_trace_context, "parent_span_id"));
    TEST_ASSERT(!!event_trace_parent_span_id);
    TEST_CHECK_STRING_EQUAL(event_trace_parent_span_id, parent_span_id);

    sentry_uuid_t event_trace_span_id = sentry__value_as_uuid(
        sentry_value_get_by_key(event_trace_context, "span_id"));
    TEST_CHECK(!sentry_uuid_is_nil(&event_trace_span_id));

    sentry_value_decref(event);
}

SENTRY_TEST(scoped_txn)
{
    // initialize SDK so we have a scope
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_options_set_sample_rate(options, 1.0);
    sentry_init(options);

    // inject a trace via trace-header into a transaction
    const char *trace_header
        = "2674eb52d5874b13b560236d6c79ce8a-a0f9fdf04f1a63df-1";
    const char *txn_trace_id = "2674eb52d5874b13b560236d6c79ce8a";
    const char *txn_parent_span_id = "a0f9fdf04f1a63df";
    sentry_transaction_context_t *tx_ctx_scoped
        = sentry_transaction_context_new("wow!", NULL);
    TEST_ASSERT(!!tx_ctx_scoped);
    sentry_transaction_context_update_from_header(
        tx_ctx_scoped, "sentry-trace", trace_header);
    sentry_transaction_t *tx_scoped
        = sentry_transaction_start(tx_ctx_scoped, sentry_value_new_null());
    TEST_ASSERT(!!tx_scoped);

    // when no set_trace was called yet, the scoped transaction should apply
    //  its trace/parent span ID (as set by update_from_header)
    sentry_set_transaction_object(tx_scoped);

    apply_scope_and_check_trace_context(
        options, txn_trace_id, txn_parent_span_id);
    sentry_transaction_finish(tx_scoped);

    sentry_close();
}

SENTRY_TEST(set_trace_id_before_scoped_txn)
{
    // initialize SDK so we have a scope
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_options_set_sample_rate(options, 1.0);
    sentry_init(options);

    const char *trace_header
        = "2674eb52d5874b13b560236d6c79ce8a-a0f9fdf04f1a63df-1";
    const char *txn_trace_id = "2674eb52d5874b13b560236d6c79ce8a";
    const char *txn_parent_span_id = "a0f9fdf04f1a63df";

    sentry_transaction_context_t *tx_ctx
        = sentry_transaction_context_new("wow!", NULL);
    TEST_ASSERT(!!tx_ctx);
    sentry_transaction_context_update_from_header(
        tx_ctx, "sentry-trace", trace_header);
    sentry_transaction_t *tx
        = sentry_transaction_start(tx_ctx, sentry_value_new_null());
    TEST_ASSERT(!!tx);
    sentry_span_t *span_child
        = sentry_transaction_start_child(tx, "op", "desc");
    TEST_ASSERT(!!span_child);
    sentry_span_t *span_grandchild
        = sentry_span_start_child(span_child, "op_g", "desc_g");
    TEST_ASSERT(!!span_grandchild);

    // set the direct trace
    const char *direct_trace_id = "aaaabbbbccccddddeeeeffff00001111";
    const char *direct_parent_span_id = "f0f0f0f0f0f0f0f0";
    sentry_set_trace(direct_trace_id, direct_parent_span_id);

    // events should get that trace applied if there is no scoped span
    apply_scope_and_check_trace_context(
        options, direct_trace_id, direct_parent_span_id);

    // now set transaction to be scoped. It should keep the trace_id it had
    // before
    sentry_set_transaction_object(tx);

    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(tx->inner, "trace_id")),
        txn_trace_id);

    sentry_set_span(span_child);
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                span_child->inner, "trace_id")),
        txn_trace_id);

    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                span_grandchild->inner, "trace_id")),
        txn_trace_id);

    // get span_ids from all tx/spans
    const char *tx_span_id
        = sentry_value_as_string(sentry_value_get_by_key(tx->inner, "span_id"));
    TEST_ASSERT(!!tx_span_id);

    const char *tx_trace_id = sentry_value_as_string(
        sentry_value_get_by_key(tx->inner, "trace_id"));
    TEST_ASSERT(!!tx_trace_id);

    const char *span_child_span_id = sentry_value_as_string(
        sentry_value_get_by_key(span_child->inner, "span_id"));
    TEST_ASSERT(!!span_child_span_id);
    const char *span_child_parent_span_id = sentry_value_as_string(
        sentry_value_get_by_key(span_child->inner, "parent_span_id"));
    TEST_ASSERT(!!span_child_parent_span_id);

    const char *span_grandchild_parent_span_id = sentry_value_as_string(
        sentry_value_get_by_key(span_grandchild->inner, "parent_span_id"));
    TEST_ASSERT(!!span_grandchild_parent_span_id);

    // check if (set_trace)->root->child->grandchild is connected
    // parent_span_id should still be the one from update_from_header
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                tx->inner, "parent_span_id")),
        txn_parent_span_id);
    TEST_CHECK_STRING_EQUAL(tx_span_id, span_child_parent_span_id); // span->tx
    TEST_CHECK_STRING_EQUAL(span_child_span_id,
        span_grandchild_parent_span_id); // grandchild->child

    // since we have a scoped tx, the event should NOT get the set_trace data
    //  but the data from the scoped span
    apply_scope_and_check_trace_context(options, tx_trace_id, tx_span_id);

    sentry_span_finish(span_grandchild);
    sentry_span_finish(span_child);
    sentry_transaction_finish(tx);

    // after finishing the transaction, the direct trace should hit again
    apply_scope_and_check_trace_context(
        options, direct_trace_id, direct_parent_span_id);

    sentry_close();
}

SENTRY_TEST(set_trace_id_with_txn)
{
    // initialize SDK so we have a scope
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_options_set_sample_rate(options, 1.0);
    sentry_init(options);

    // set the direct trace before starting any spans
    const char *direct_trace_id = "aaaabbbbccccddddeeeeffff00001111";
    const char *direct_parent_span_id = "f0f0f0f0f0f0f0f0";
    sentry_set_trace(direct_trace_id, direct_parent_span_id);

    sentry_transaction_context_t *tx_ctx
        = sentry_transaction_context_new("wow!", NULL);
    TEST_ASSERT(!!tx_ctx);
    sentry_transaction_t *tx
        = sentry_transaction_start(tx_ctx, sentry_value_new_null());
    TEST_ASSERT(!!tx);
    sentry_span_t *span_child
        = sentry_transaction_start_child(tx, "op", "desc");
    TEST_ASSERT(!!span_child);
    sentry_span_t *span_grandchild
        = sentry_span_start_child(span_child, "op_g", "desc_g");
    TEST_ASSERT(!!span_grandchild);

    // the direct trace should apply to any span that's started after it was set
    // check if trace_id was passed down properly
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(tx->inner, "trace_id")),
        direct_trace_id);
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                span_child->inner, "trace_id")),
        direct_trace_id);
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                span_grandchild->inner, "trace_id")),
        direct_trace_id);

    const char *tx_span_id
        = sentry_value_as_string(sentry_value_get_by_key(tx->inner, "span_id"));
    TEST_ASSERT(!!tx_span_id);
    const char *span_child_span_id = sentry_value_as_string(
        sentry_value_get_by_key(span_child->inner, "span_id"));
    TEST_ASSERT(!!span_child_span_id);
    const char *span_child_parent_span_id = sentry_value_as_string(
        sentry_value_get_by_key(span_child->inner, "parent_span_id"));
    TEST_ASSERT(!!span_child_parent_span_id);
    const char *span_grandchild_parent_span_id = sentry_value_as_string(
        sentry_value_get_by_key(span_grandchild->inner, "parent_span_id"));
    TEST_ASSERT(!!span_grandchild_parent_span_id);
    // check if (set_trace)->root->child->grandchild is connected
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                tx->inner, "parent_span_id")),
        direct_parent_span_id);
    TEST_CHECK_STRING_EQUAL(tx_span_id, span_child_parent_span_id); // span->tx
    TEST_CHECK_STRING_EQUAL(span_child_span_id,
        span_grandchild_parent_span_id); // grandchild->child

    sentry_span_finish(span_grandchild);
    sentry_span_finish(span_child);
    sentry_transaction_finish(tx);
    // events should get set_trace data applied if there is no scoped span
    apply_scope_and_check_trace_context(
        options, direct_trace_id, direct_parent_span_id);

    sentry_close();
}

SENTRY_TEST(set_trace_update_from_header)
{
    // initialize SDK so we have a scope
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_options_set_sample_rate(options, 1.0);
    sentry_init(options);

    // set the direct trace before starting any spans
    const char *direct_trace_id = "aaaabbbbccccddddeeeeffff00001111";
    const char *direct_parent_span_id = "f0f0f0f0f0f0f0f0";
    sentry_set_trace(direct_trace_id, direct_parent_span_id);

    // inject a trace via trace-header into a transaction
    const char *trace_header
        = "2674eb52d5874b13b560236d6c79ce8a-a0f9fdf04f1a63df-1";
    const char *txn_trace_id = "2674eb52d5874b13b560236d6c79ce8a";
    const char *txn_parent_span_id = "a0f9fdf04f1a63df";
    sentry_transaction_context_t *tx_ctx
        = sentry_transaction_context_new("wow!", NULL);
    TEST_ASSERT(!!tx_ctx);
    sentry_transaction_context_update_from_header(
        tx_ctx, "sentry-trace", trace_header);
    sentry_transaction_t *tx
        = sentry_transaction_start(tx_ctx, sentry_value_new_null());
    TEST_ASSERT(!!tx);
    sentry_span_t *span_child
        = sentry_transaction_start_child(tx, "op", "desc");
    TEST_ASSERT(!!span_child);

    // check that trace_header data is applied (and not set_trace data)
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(tx->inner, "trace_id")),
        txn_trace_id);
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                span_child->inner, "trace_id")),
        txn_trace_id);
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                tx->inner, "parent_span_id")),
        txn_parent_span_id);

    // events should get set_trace data applied if there is no scoped span
    apply_scope_and_check_trace_context(
        options, direct_trace_id, direct_parent_span_id);

    sentry_span_finish(span_child);
    sentry_transaction_finish(tx);

    sentry_close();
}

SENTRY_TEST(set_trace_id_twice)
{
    // initialize SDK so we have a scope
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_options_set_sample_rate(options, 1.0);
    sentry_init(options);

    // set the first direct trace
    const char *direct_trace_id = "aaaabbbbccccddddeeeeffff00001111";
    const char *direct_parent_span_id = "f0f0f0f0f0f0f0f0";
    sentry_set_trace(direct_trace_id, direct_parent_span_id);

    apply_scope_and_check_trace_context(
        options, direct_trace_id, direct_parent_span_id);

    // set second direct trace
    const char *direct_trace_id_2 = "11110000ffffeeeeddddccccbbbbaaaa";
    const char *direct_parent_span_id_2 = "a9a9a9a9a9a9a9a9";
    sentry_set_trace(direct_trace_id_2, direct_parent_span_id_2);

    apply_scope_and_check_trace_context(
        options, direct_trace_id_2, direct_parent_span_id_2);

    sentry_close();
}

SENTRY_TEST(propagation_context_init)
{
    // initialize SDK so we have a scope
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_traces_sample_rate(options, 1.0);
    sentry_options_set_sample_rate(options, 1.0);
    sentry_init(options);

    sentry_transaction_context_t *tx_ctx
        = sentry_transaction_context_new("wow!", NULL);
    TEST_ASSERT(!!tx_ctx);
    sentry_transaction_t *tx
        = sentry_transaction_start(tx_ctx, sentry_value_new_null());
    TEST_ASSERT(!!tx);
    sentry_span_t *span_child
        = sentry_transaction_start_child(tx, "op", "desc");
    TEST_ASSERT(!!span_child);

    const char *propagation_context_trace_id = sentry_value_as_string(
        sentry_value_get_by_key(tx->inner, "trace_id"));
    TEST_ASSERT(!!propagation_context_trace_id);
    // on SDK init, propagation_context is set with a trace_id and span_id
    // the trace_id is used for both events and spans
    apply_scope_and_check_trace_context(
        options, propagation_context_trace_id, "");

    sentry_span_finish(span_child);
    sentry_transaction_finish(tx);
    sentry_close();
}

#undef IS_NULL
#undef CHECK_STRING_PROPERTY
