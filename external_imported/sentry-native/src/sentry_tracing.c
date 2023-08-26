#include "sentry_tracing.h"
#include "sentry_alloc.h"
#include "sentry_logger.h"
#include "sentry_string.h"
#include "sentry_utils.h"
#include "sentry_value.h"
#include <string.h>

sentry_value_t
sentry__value_new_span(sentry_value_t parent, const char *operation)
{
    sentry_value_t span = sentry_value_new_object();

    sentry_value_set_by_key(span, "op", sentry_value_new_string(operation));

    sentry_uuid_t span_id = sentry_uuid_new_v4();
    sentry_value_set_by_key(
        span, "span_id", sentry__value_new_span_uuid(&span_id));

    sentry_value_set_by_key(span, "status", sentry_value_new_string("ok"));

    if (!sentry_value_is_null(parent)) {
        sentry_value_set_by_key(span, "trace_id",
            sentry_value_get_by_key_owned(parent, "trace_id"));
        sentry_value_set_by_key(span, "parent_span_id",
            sentry_value_get_by_key_owned(parent, "span_id"));
        sentry_value_set_by_key(
            span, "sampled", sentry_value_get_by_key_owned(parent, "sampled"));
    }

    return span;
}

sentry_value_t
sentry__value_transaction_context_new(const char *name, const char *operation)
{
    sentry_value_t transaction_context
        = sentry__value_new_span(sentry_value_new_null(), operation);

    sentry_uuid_t trace_id = sentry_uuid_new_v4();
    sentry_value_set_by_key(transaction_context, "trace_id",
        sentry__value_new_internal_uuid(&trace_id));

    sentry_value_set_by_key(
        transaction_context, "transaction", sentry_value_new_string(name));

    return transaction_context;
}

sentry_transaction_context_t *
sentry_transaction_context_new(const char *name, const char *operation)
{
    sentry_transaction_context_t *tx_cxt
        = SENTRY_MAKE(sentry_transaction_context_t);
    if (!tx_cxt) {
        return NULL;
    }
    tx_cxt->inner = sentry__value_transaction_context_new(name, operation);

    if (sentry_value_is_null(tx_cxt->inner)) {
        sentry_free(tx_cxt);
        return NULL;
    }

    return tx_cxt;
}

void
sentry__transaction_context_free(sentry_transaction_context_t *tx_cxt)
{
    if (!tx_cxt) {
        return;
    }
    if (sentry_value_refcount(tx_cxt->inner) <= 1) {
        sentry_value_decref(tx_cxt->inner);
        sentry_free(tx_cxt);
    } else {
        sentry_value_decref(tx_cxt->inner);
    };
}

void
sentry_transaction_context_set_name(
    sentry_transaction_context_t *tx_cxt, const char *name)
{
    if (tx_cxt) {
        sentry_value_set_by_key(
            tx_cxt->inner, "transaction", sentry_value_new_string(name));
    }
}

void
sentry_transaction_context_set_operation(
    sentry_transaction_context_t *tx_cxt, const char *operation)
{
    if (tx_cxt) {
        sentry_value_set_by_key(
            tx_cxt->inner, "op", sentry_value_new_string(operation));
    }
}

void
sentry_transaction_context_set_sampled(
    sentry_transaction_context_t *tx_cxt, int sampled)
{
    if (tx_cxt) {
        sentry_value_set_by_key(
            tx_cxt->inner, "sampled", sentry_value_new_bool(sampled));
    }
}

void
sentry_transaction_context_remove_sampled(sentry_transaction_context_t *tx_cxt)
{
    if (tx_cxt) {
        sentry_value_remove_by_key(tx_cxt->inner, "sampled");
    }
}

void
sentry_transaction_context_update_from_header(
    sentry_transaction_context_t *tx_cxt, const char *key, const char *value)
{
    if (!tx_cxt) {
        return;
    }

    // do case-insensitive header key comparison
    const char sentry_trace[] = "sentry-trace";
    for (size_t i = 0; i < sizeof(sentry_trace); i++) {
        if (tolower(key[i]) != sentry_trace[i]) {
            return;
        }
    }

    // https://develop.sentry.dev/sdk/performance/#header-sentry-trace
    // sentry-trace = traceid-spanid(-sampled)?
    const char *trace_id_start = value;
    const char *trace_id_end = strchr(trace_id_start, '-');
    if (!trace_id_end) {
        return;
    }

    sentry_value_t inner = tx_cxt->inner;

    char *s
        = sentry__string_clonen(trace_id_start, trace_id_end - trace_id_start);
    sentry_value_t trace_id = sentry__value_new_string_owned(s);
    sentry_value_set_by_key(inner, "trace_id", trace_id);

    const char *span_id_start = trace_id_end + 1;
    const char *span_id_end = strchr(span_id_start, '-');
    if (!span_id_end) {
        // no sampled flag
        sentry_value_t parent_span_id = sentry_value_new_string(span_id_start);
        sentry_value_set_by_key(inner, "parent_span_id", parent_span_id);
        return;
    }
    // else: we have a sampled flag

    s = sentry__string_clonen(span_id_start, span_id_end - span_id_start);
    sentry_value_t parent_span_id = sentry__value_new_string_owned(s);
    sentry_value_set_by_key(inner, "parent_span_id", parent_span_id);

    bool sampled = *(span_id_end + 1) == '1';
    sentry_value_set_by_key(inner, "sampled", sentry_value_new_bool(sampled));
}

sentry_transaction_t *
sentry__transaction_new(sentry_value_t inner)
{
    if (sentry_value_is_null(inner)) {
        return NULL;
    }

    sentry_transaction_t *tx = SENTRY_MAKE(sentry_transaction_t);
    if (!tx) {
        return NULL;
    }

    tx->inner = inner;

    return tx;
}

void
sentry__transaction_incref(sentry_transaction_t *tx)
{
    if (tx) {
        sentry_value_incref(tx->inner);
    }
}

void
sentry__transaction_decref(sentry_transaction_t *tx)
{
    if (!tx) {
        return;
    }

    if (sentry_value_refcount(tx->inner) <= 1) {
        sentry_value_decref(tx->inner);
        sentry_free(tx);
    } else {
        sentry_value_decref(tx->inner);
    };
}

void
sentry__span_incref(sentry_span_t *span)
{
    if (span) {
        sentry_value_incref(span->inner);
    }
}

void
sentry__span_decref(sentry_span_t *span)
{
    if (!span) {
        return;
    }

    if (sentry_value_refcount(span->inner) <= 1) {
        sentry_value_decref(span->inner);
        sentry__transaction_decref(span->transaction);
        sentry_free(span);
    } else {
        sentry_value_decref(span->inner);
    };
}

sentry_span_t *
sentry__span_new(sentry_transaction_t *tx, sentry_value_t inner)
{
    if (!tx || sentry_value_is_null(inner)) {
        return NULL;
    }

    sentry_span_t *span = SENTRY_MAKE(sentry_span_t);
    if (!span) {
        return NULL;
    }

    span->inner = inner;

    sentry__transaction_incref(tx);
    span->transaction = tx;

    return span;
}

sentry_value_t
sentry__value_span_new(
    size_t max_spans, sentry_value_t parent, char *operation, char *description)
{
    if (!sentry_value_is_null(sentry_value_get_by_key(parent, "timestamp"))) {
        SENTRY_DEBUG("span's parent is already finished, not creating span");
        goto fail;
    }

    sentry_value_t spans = sentry_value_get_by_key(parent, "spans");
    // This only checks that the number of _completed_ spans matches the
    // number of max spans. This means that the number of in-flight spans
    // can exceed the max number of spans.
    if (sentry_value_get_length(spans) >= max_spans) {
        SENTRY_DEBUG("reached maximum number of spans for transaction, not "
                     "creating span");
        goto fail;
    }

    sentry_value_t child = sentry__value_new_span(parent, operation);
    sentry_value_set_by_key(
        child, "description", sentry_value_new_string(description));
    sentry_value_set_by_key(child, "start_timestamp",
        sentry__value_new_string_owned(
            sentry__msec_time_to_iso8601(sentry__msec_time())));

    return child;
fail:
    return sentry_value_new_null();
}

sentry_value_t
sentry__value_get_trace_context(sentry_value_t span)
{
    if (sentry_value_is_null(span)) {
        return sentry_value_new_null();
    }

    if (sentry_value_is_null(sentry_value_get_by_key(span, "trace_id"))
        || sentry_value_is_null(sentry_value_get_by_key(span, "span_id"))) {
        return sentry_value_new_null();
    }

    sentry_value_t trace_context = sentry_value_new_object();

#define PLACE_VALUE(Key, Source)                                               \
    do {                                                                       \
        sentry_value_t src = sentry_value_get_by_key(Source, Key);             \
        if (!sentry_value_is_null(src)) {                                      \
            sentry_value_incref(src);                                          \
            sentry_value_set_by_key(trace_context, Key, src);                  \
        }                                                                      \
    } while (0)

    PLACE_VALUE("trace_id", span);
    PLACE_VALUE("span_id", span);
    PLACE_VALUE("parent_span_id", span);
    PLACE_VALUE("op", span);
    PLACE_VALUE("description", span);
    PLACE_VALUE("status", span);

    // TODO: freeze this
    return trace_context;

#undef PLACE_VALUE
}

void
sentry_transaction_set_name(sentry_transaction_t *tx, const char *name)
{
    if (tx) {
        sentry_value_set_by_key(
            tx->inner, "transaction", sentry_value_new_string(name));
    }
}

static void
set_tag(sentry_value_t item, const char *tag, const char *value)
{
    sentry_value_t tags = sentry_value_get_by_key(item, "tags");
    if (sentry_value_is_null(tags)) {
        tags = sentry_value_new_object();
        sentry_value_set_by_key(item, "tags", tags);
    }

    char *s = sentry__string_clonen(value, 200);
    if (s) {
        sentry_value_set_by_key(tags, tag, sentry__value_new_string_owned(s));
    } else {
        sentry_value_set_by_key(tags, tag, sentry_value_new_null());
    }
}

void
sentry_transaction_set_tag(
    sentry_transaction_t *tx, const char *tag, const char *value)
{
    if (tx) {
        set_tag(tx->inner, tag, value);
    }
}

void
sentry_span_set_tag(sentry_span_t *span, const char *tag, const char *value)
{
    if (span) {
        set_tag(span->inner, tag, value);
    }
}

static void
remove_tag(sentry_value_t item, const char *tag)
{
    sentry_value_t tags = sentry_value_get_by_key(item, "tags");
    if (!sentry_value_is_null(tags)) {
        sentry_value_remove_by_key(tags, tag);
    }
}

void
sentry_transaction_remove_tag(sentry_transaction_t *tx, const char *tag)
{
    if (tx) {
        remove_tag(tx->inner, tag);
    }
}

void
sentry_span_remove_tag(sentry_span_t *span, const char *tag)
{
    if (span) {
        remove_tag(span->inner, tag);
    }
}

static void
set_data(sentry_value_t item, const char *key, sentry_value_t value)
{
    sentry_value_t data = sentry_value_get_by_key(item, "data");
    if (sentry_value_is_null(data)) {
        data = sentry_value_new_object();
        sentry_value_set_by_key(item, "data", data);
    }
    sentry_value_set_by_key(data, key, value);
}

void
sentry_transaction_set_data(
    sentry_transaction_t *tx, const char *key, sentry_value_t value)
{
    if (tx) {
        set_data(tx->inner, key, value);
    }
}

void
sentry_span_set_data(sentry_span_t *span, const char *key, sentry_value_t value)
{
    if (span) {
        set_data(span->inner, key, value);
    }
}

static void
remove_data(sentry_value_t item, const char *key)
{
    sentry_value_t data = sentry_value_get_by_key(item, "data");
    if (!sentry_value_is_null(data)) {
        sentry_value_remove_by_key(data, key);
    }
}

void
sentry_transaction_remove_data(sentry_transaction_t *tx, const char *key)
{
    if (tx) {
        remove_data(tx->inner, key);
    }
}

void
sentry_span_remove_data(sentry_span_t *span, const char *key)
{
    if (span) {
        remove_data(span->inner, key);
    }
}

sentry_value_t
sentry_status_to_string(sentry_span_status_t status)
{
    switch (status) {
    case SENTRY_SPAN_STATUS_OK:
        return sentry_value_new_string("ok");
    case SENTRY_SPAN_STATUS_CANCELLED:
        return sentry_value_new_string("cancelled");
    case SENTRY_SPAN_STATUS_UNKNOWN:
        return sentry_value_new_string("unknown");
    case SENTRY_SPAN_STATUS_INVALID_ARGUMENT:
        return sentry_value_new_string("invalid_argument");
    case SENTRY_SPAN_STATUS_DEADLINE_EXCEEDED:
        return sentry_value_new_string("deadline_exceeded");
    case SENTRY_SPAN_STATUS_NOT_FOUND:
        return sentry_value_new_string("not_found");
    case SENTRY_SPAN_STATUS_ALREADY_EXISTS:
        return sentry_value_new_string("already_exists");
    case SENTRY_SPAN_STATUS_PERMISSION_DENIED:
        return sentry_value_new_string("permission_denied");
    case SENTRY_SPAN_STATUS_RESOURCE_EXHAUSTED:
        return sentry_value_new_string("resource_exhausted");
    case SENTRY_SPAN_STATUS_FAILED_PRECONDITION:
        return sentry_value_new_string("failed_precondition");
    case SENTRY_SPAN_STATUS_ABORTED:
        return sentry_value_new_string("aborted");
    case SENTRY_SPAN_STATUS_OUT_OF_RANGE:
        return sentry_value_new_string("out_of_range");
    case SENTRY_SPAN_STATUS_UNIMPLEMENTED:
        return sentry_value_new_string("unimplemented");
    case SENTRY_SPAN_STATUS_INTERNAL_ERROR:
        return sentry_value_new_string("internal_error");
    case SENTRY_SPAN_STATUS_UNAVAILABLE:
        return sentry_value_new_string("unavailable");
    case SENTRY_SPAN_STATUS_DATA_LOSS:
        return sentry_value_new_string("data_loss");
    case SENTRY_SPAN_STATUS_UNAUTHENTICATED:
        return sentry_value_new_string("unauthenticated");
    default:
        return sentry_value_new_null();
    }
}

void
set_status(sentry_value_t item, sentry_span_status_t status)
{
    sentry_value_set_by_key(item, "status", sentry_status_to_string(status));
}

void
sentry_span_set_status(sentry_span_t *span, sentry_span_status_t status)
{
    if (span) {
        set_status(span->inner, status);
    }
}

void
sentry_transaction_set_status(
    sentry_transaction_t *tx, sentry_span_status_t status)
{
    if (tx) {
        set_status(tx->inner, status);
    }
}

static void
sentry__span_iter_headers(sentry_value_t span,
    sentry_iter_headers_function_t callback, void *userdata)
{
    sentry_value_t trace_id = sentry_value_get_by_key(span, "trace_id");
    sentry_value_t span_id = sentry_value_get_by_key(span, "span_id");
    sentry_value_t sampled = sentry_value_get_by_key(span, "sampled");

    if (sentry_value_is_null(trace_id) || sentry_value_is_null(span_id)) {
        return;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%s-%s-%s", sentry_value_as_string(trace_id),
        sentry_value_as_string(span_id),
        sentry_value_is_true(sampled) ? "1" : "0");

    callback("sentry-trace", buf, userdata);
}

void
sentry_span_iter_headers(sentry_span_t *span,
    sentry_iter_headers_function_t callback, void *userdata)
{
    if (span) {
        sentry__span_iter_headers(span->inner, callback, userdata);
    }
}

void
sentry_transaction_iter_headers(sentry_transaction_t *tx,
    sentry_iter_headers_function_t callback, void *userdata)
{
    if (tx) {
        sentry__span_iter_headers(tx->inner, callback, userdata);
    }
}
