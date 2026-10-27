#ifndef SENTRY_TRACING_H_INCLUDED
#define SENTRY_TRACING_H_INCLUDED

#include "sentry_slice.h"
#include "sentry_sync.h"
#include "sentry_value.h"

// W3C traceparent header: 00-<traceId>-<spanId>-<flags>
// length: 00-32char-16char-02char
#define SENTRY_W3C_TRACEPARENT_LEN 55

// sentry-trace header: <traceId>-<spanId>-<sampled>
// length: 32char-16char-01char
#define SENTRY_TRACE_LEN 51

/**
 * A span.
 */
struct sentry_span_s {
    sentry_value_t inner;
    // The transaction the span is contained in.
    sentry_transaction_t *transaction;
};

/**
 * A transaction context.
 */
struct sentry_transaction_context_s {
    sentry_value_t inner;
};

/**
 * A transaction.
 */
struct sentry_transaction_s {
    sentry_value_t inner;
    // Live (unfinished) child spans, so `sentry__trace_finish` can close them
    // out on crash. Weak pointers: entries do not own a ref — spans remove
    // themselves via `sentry__transaction_remove_child` on finish or decref.
    sentry_mutex_t children_mutex;
    sentry_span_t **children;
    size_t children_count;
    size_t children_cap;
};

void sentry__transaction_context_free(sentry_transaction_context_t *tx_ctx);

sentry_transaction_t *sentry__transaction_new(sentry_value_t inner);
void sentry__transaction_incref(sentry_transaction_t *tx);
void sentry__transaction_decref(sentry_transaction_t *tx);

/**
 * Unlists `span` from the transaction's live-children list. No-op if not
 * found. Does not decref (the list holds weak pointers).
 */
void sentry__transaction_remove_child(
    sentry_transaction_t *tx, sentry_span_t *span);

/**
 * Finishes the active transaction (if any) with `status`, closing out every
 * in-flight child span in leaf-first order and returning the tx payload.
 * `scope->span` / `scope->transaction_object` are preserved so a
 * subsequently-captured crash event still inherits the active trace context.
 * Returns null if nothing is active.
 */
sentry_value_t sentry__trace_finish(sentry_span_status_t status);

void sentry__span_incref(sentry_span_t *span);
void sentry__span_decref(sentry_span_t *span);

sentry_value_t sentry__value_span_new(size_t max_spans, sentry_value_t parent,
    const char *operation, const char *description, uint64_t timestamp);
sentry_value_t sentry__value_span_new_n(size_t max_spans, sentry_value_t parent,
    sentry_slice_t operation, sentry_slice_t description, uint64_t timestamp);

sentry_span_t *sentry__span_new(
    sentry_transaction_t *parent_tx, sentry_value_t inner);

/**
 * Returns an object containing tracing information extracted from a
 * transaction / span which should be included in an event.
 * See https://develop.sentry.dev/sdk/event-payloads/transaction/#examples
 */
sentry_value_t sentry__value_get_trace_context(sentry_value_t span);

/**
 * Returns whether to continue an incoming trace given the SDK options and the
 * incoming dynamic sampling context.
 *
 * See
 * https://develop.sentry.dev/sdk/foundations/trace-propagation/#strict-trace-continuation
 */
bool sentry__trace_can_continue(
    sentry_value_t incoming, const sentry_options_t *options);

#endif
