#include "sentry_tracing.h"
#include "sentry.h"
#include "sentry_alloc.h"
#include "sentry_core.h"
#include "sentry_logger.h"
#include "sentry_options.h"
#include "sentry_scope.h"
#include "sentry_slice.h"
#include "sentry_string.h"
#include "sentry_utils.h"
#include "sentry_value.h"
#include <ctype.h>
#include <string.h>

static inline bool
isalnum_c(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z');
}

static void
percent_encode_append(sentry_stringbuilder_t *sb, const char *value)
{
    // Encode every byte that isn't an RFC 3986 unreserved character
    // (ALPHA / DIGIT / "-" / "." / "_" / "~") as %XX.
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        unsigned char c = *p;
        if (isalnum_c(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            sentry__stringbuilder_append_char(sb, (char)c);
        } else {
            char esc[3] = { '%', hex[c >> 4], hex[c & 0xF] };
            sentry__stringbuilder_append_buf(sb, esc, 3);
        }
    }
}

static void
append_baggage_member(const char *key, sentry_value_t value, void *userdata)
{
    if (!key || strcmp(key, "trace_id") == 0 || sentry_value_is_null(value)) {
        return;
    }

    char *value_str = sentry__value_stringify(value);
    if (!value_str) {
        return;
    }

    sentry_stringbuilder_t *sb = userdata;
    sentry__stringbuilder_append(sb, ",sentry-");
    sentry__stringbuilder_append(sb, key);
    sentry__stringbuilder_append_char(sb, '=');
    percent_encode_append(sb, value_str);
    sentry_free(value_str);
}

static sentry_value_t
new_span_n(sentry_value_t parent, sentry_slice_t operation)
{
    sentry_value_t span = sentry_value_new_object();

    sentry_value_set_by_key(
        span, "op", sentry_value_new_string_n(operation.ptr, operation.len));

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

static sentry_value_t
transaction_context_new_n(sentry_slice_t name, sentry_slice_t operation)
{
    sentry_value_t transaction_context
        = new_span_n(sentry_value_new_null(), operation);

    sentry_uuid_t trace_id = sentry_uuid_new_v4();
    sentry_value_set_by_key(transaction_context, "trace_id",
        sentry__value_new_internal_uuid(&trace_id));

    sentry_value_set_by_key(transaction_context, "transaction",
        sentry_value_new_string_n(name.ptr, name.len));

    SENTRY_WITH_SCOPE_MUT (scope) {
        if (!scope->trace_managed
            && !sentry_value_is_null(
                sentry_value_get_by_key(scope->propagation_context, "trace"))) {
            // The trace is managed from outside, so we use the propagation
            // context as the trace source for this transaction. This means that
            // either a downstream SDK or the user manages trace life-cycles.
            sentry_value_set_by_key(transaction_context, "trace_id",
                sentry__value_clone(sentry_value_get_by_key(
                    sentry_value_get_by_key(
                        scope->propagation_context, "trace"),
                    "trace_id")));
            sentry_value_set_by_key(transaction_context, "parent_span_id",
                sentry__value_clone(sentry_value_get_by_key(
                    sentry_value_get_by_key(
                        scope->propagation_context, "trace"),
                    "parent_span_id")));
        }
    }

    return transaction_context;
}

sentry_transaction_context_t *
sentry_transaction_context_new_n(const char *name, size_t name_len,
    const char *operation, size_t operation_len)
{
    sentry_transaction_context_t *tx_ctx
        = SENTRY_MAKE(sentry_transaction_context_t);
    if (!tx_ctx) {
        return NULL;
    }
    tx_ctx->inner
        = transaction_context_new_n((sentry_slice_t) { name, name_len },
            (sentry_slice_t) { operation, operation_len });

    if (sentry_value_is_null(tx_ctx->inner)) {
        sentry_free(tx_ctx);
        return NULL;
    }

    return tx_ctx;
}

sentry_transaction_context_t *
sentry_transaction_context_new(const char *name, const char *operation)
{
    return sentry_transaction_context_new_n(name, sentry__guarded_strlen(name),
        operation, sentry__guarded_strlen(operation));
}

void
sentry__transaction_context_free(sentry_transaction_context_t *tx_ctx)
{
    if (!tx_ctx) {
        return;
    }
    if (!sentry_value_decref(tx_ctx->inner)) {
        sentry_free(tx_ctx);
    }
}

void
sentry_transaction_context_set_name(
    sentry_transaction_context_t *tx_ctx, const char *name)
{
    if (tx_ctx) {
        sentry_value_set_by_key(
            tx_ctx->inner, "transaction", sentry_value_new_string(name));
    }
}

void
sentry_transaction_context_set_name_n(
    sentry_transaction_context_t *tx_ctx, const char *name, size_t name_len)
{
    if (tx_ctx) {
        sentry_value_set_by_key(tx_ctx->inner, "transaction",
            sentry_value_new_string_n(name, name_len));
    }
}

const char *
sentry_transaction_context_get_name(const sentry_transaction_context_t *tx_ctx)
{
    return sentry_value_as_string(
        sentry_value_get_by_key(tx_ctx->inner, "transaction"));
}

void
sentry_transaction_context_set_operation(
    sentry_transaction_context_t *tx_ctx, const char *operation)
{
    if (tx_ctx) {
        sentry_value_set_by_key(
            tx_ctx->inner, "op", sentry_value_new_string(operation));
    }
}

void
sentry_transaction_context_set_operation_n(sentry_transaction_context_t *tx_ctx,
    const char *operation, size_t operation_len)
{
    if (tx_ctx) {
        sentry_value_set_by_key(tx_ctx->inner, "op",
            sentry_value_new_string_n(operation, operation_len));
    }
}

const char *
sentry_transaction_context_get_operation(
    const sentry_transaction_context_t *tx_ctx)
{
    return sentry_value_as_string(sentry_value_get_by_key(tx_ctx->inner, "op"));
}

void
sentry_transaction_context_set_sampled(
    sentry_transaction_context_t *tx_ctx, int sampled)
{
    if (tx_ctx) {
        sentry_value_set_by_key(
            tx_ctx->inner, "sampled", sentry_value_new_bool(sampled));
    }
}

void
sentry_transaction_context_remove_sampled(sentry_transaction_context_t *tx_ctx)
{
    if (tx_ctx) {
        sentry_value_remove_by_key(tx_ctx->inner, "sampled");
    }
}

/*
 * Checks whether the string is a valid hex string over the given length and
 * contains at least one non-zero character.
 */
static bool
is_valid_nonzero_hexstring(const char *s, size_t len)
{
    bool has_nonzero = false;
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit(s[i])) {
            return false;
        }
        if (s[i] != '0') {
            has_nonzero = true;
        }
    }
    return has_nonzero;
}

static bool
is_valid_id(const char *id, size_t expected_len, const char *id_type)
{
    const bool is_valid = id != NULL && strlen(id) == expected_len
        && is_valid_nonzero_hexstring(id, expected_len);

    if (!is_valid) {
        SENTRY_WARNF("invalid %s format in given header", id_type);
    }

    return is_valid;
}

static bool
is_valid_trace_id(const char *trace_id)
{
    return is_valid_id(trace_id, 32, "trace id");
}

static bool
is_valid_span_id(const char *span_id)
{
    return is_valid_id(span_id, 16, "span id");
}

static bool
compare_header_key(
    const char *key, size_t key_len, const char *expected, size_t expected_len)
{
    if (key_len != expected_len) {
        return false;
    }
    for (size_t i = 0; i < expected_len; i++) {
        if (tolower(key[i]) != expected[i]) {
            return false;
        }
    }
    return true;
}

static void
parse_sentry_trace(
    sentry_transaction_context_t *tx_ctx, const char *value, size_t value_len)
{
    // Parse sentry-trace header: traceid-spanid(-sampled)?
    const char *trace_id_start = value;
    const char *trace_id_end = memchr(trace_id_start, '-', value_len);
    if (!trace_id_end) {
        SENTRY_WARN("invalid trace id format in given header");
        return;
    }

    sentry_value_t inner = tx_ctx->inner;

    char *s = sentry__string_clone_n(
        trace_id_start, (size_t)(trace_id_end - trace_id_start));
    if (!is_valid_trace_id(s)) {
        sentry_free(s);
        return;
    }
    sentry_value_t trace_id = sentry__value_new_string_owned(s);
    sentry_value_set_by_key(inner, "trace_id", trace_id);

    // Mark that an upstream trace was received. `incoming_dsc` doubles as this
    // marker so the strict-continuation check fires even when no `baggage`
    // arrives; baggage parsing merges into the same object regardless of
    // header order.
    if (sentry_value_is_null(sentry_value_get_by_key(inner, "incoming_dsc"))) {
        sentry_value_set_by_key(
            inner, "incoming_dsc", sentry_value_new_object());
    }

    const char *span_id_start = trace_id_end + 1;
    const char *value_end = value + value_len;
    const char *span_id_end
        = memchr(span_id_start, '-', (size_t)(value_end - span_id_start));
    const size_t span_id_len
        = (size_t)((span_id_end ? span_id_end : value_end) - span_id_start);
    if (!span_id_end) {
        // no sampled flag
        sentry_value_t parent_span_id
            = sentry_value_new_string_n(span_id_start, span_id_len);
        if (!is_valid_span_id(sentry_value_as_string(parent_span_id))) {
            sentry_value_decref(parent_span_id);
            return;
        }
        sentry_value_set_by_key(inner, "parent_span_id", parent_span_id);
        return;
    }
    // else: we have a sampled flag

    s = sentry__string_clone_n(span_id_start, span_id_len);
    if (!is_valid_span_id(s)) {
        sentry_free(s);
        return;
    }
    sentry_value_t parent_span_id = sentry__value_new_string_owned(s);
    sentry_value_set_by_key(inner, "parent_span_id", parent_span_id);

    bool sampled = span_id_end + 1 < value_end && *(span_id_end + 1) == '1';
    sentry_value_set_by_key(inner, "sampled", sentry_value_new_bool(sampled));
}

static void
parse_baggage(
    sentry_transaction_context_t *tx_ctx, const char *value, size_t value_len)
{
    // https://www.w3.org/TR/baggage/ — Sentry-prefixed members are kept and
    // percent-decoded; non-sentry members are ignored.
    static const char sentry_prefix[] = "sentry-";
    static const size_t sentry_prefix_len = sizeof(sentry_prefix) - 1;

    sentry_value_t inner = tx_ctx->inner;
    sentry_value_t incoming = sentry_value_get_by_key(inner, "incoming_dsc");
    if (sentry_value_is_null(incoming)) {
        incoming = sentry_value_new_object();
        sentry_value_set_by_key(inner, "incoming_dsc", incoming);
        incoming = sentry_value_get_by_key(inner, "incoming_dsc");
    }

    sentry_slice_t remaining = { value, value_len };
    sentry_slice_t key, val;
    while (sentry__baggage_iter_next(&remaining, &key, &val)) {
        if (key.len <= sentry_prefix_len
            || memcmp(key.ptr, sentry_prefix, sentry_prefix_len) != 0) {
            continue;
        }
        const char *sub_key = key.ptr + sentry_prefix_len;
        size_t sub_key_len = key.len - sentry_prefix_len;

        char *decoded = sentry__string_clone_n(val.ptr, val.len);
        if (!decoded) {
            continue;
        }
        size_t decoded_len = sentry__percent_decode_inplace(decoded, val.len);
        decoded[decoded_len] = '\0';
        sentry_value_set_by_key_n(incoming, sub_key, sub_key_len,
            sentry__value_new_string_owned(decoded));
    }
}

bool
sentry__trace_can_continue(
    sentry_value_t incoming, const sentry_options_t *options)
{
    const char *sdk_org_id = sentry__options_get_org_id(options);
    const char *incoming_org_id
        = sentry_value_as_string(sentry_value_get_by_key(incoming, "org_id"));
    bool sdk_has = sdk_org_id && *sdk_org_id;
    bool inc_has = incoming_org_id && *incoming_org_id;
    if (sdk_has && inc_has) {
        return strcmp(sdk_org_id, incoming_org_id) == 0;
    }
    if (sdk_has != inc_has) {
        return !options->strict_trace_continuation;
    }
    return true;
}

void
sentry_transaction_context_update_from_header_n(
    sentry_transaction_context_t *tx_ctx, const char *key, size_t key_len,
    const char *value, size_t value_len)
{
    if (!tx_ctx) {
        return;
    }

    // do case-insensitive header key comparison
    const char sentry_trace[] = "sentry-trace";
    const size_t sentry_trace_len = sizeof(sentry_trace) - 1;
    if (compare_header_key(key, key_len, sentry_trace, sentry_trace_len)) {
        parse_sentry_trace(tx_ctx, value, value_len);
        return;
    }

    const char baggage[] = "baggage";
    const size_t baggage_len = sizeof(baggage) - 1;
    if (compare_header_key(key, key_len, baggage, baggage_len)) {
        parse_baggage(tx_ctx, value, value_len);
        return;
    }
}

void
sentry_transaction_context_update_from_header(
    sentry_transaction_context_t *tx_ctx, const char *key, const char *value)
{
    sentry_transaction_context_update_from_header_n(tx_ctx, key,
        sentry__guarded_strlen(key), value, sentry__guarded_strlen(value));
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
    sentry__mutex_init(&tx->children_mutex);
    tx->children = NULL;
    tx->children_count = 0;
    tx->children_cap = 0;

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

    if (!sentry_value_decref(tx->inner)) {
        sentry_free(tx->children);
        sentry__mutex_free(&tx->children_mutex);
        sentry_free(tx);
    }
}

void
sentry__transaction_remove_child(sentry_transaction_t *tx, sentry_span_t *span)
{
    if (!tx || !span) {
        return;
    }
    sentry__mutex_lock(&tx->children_mutex);
    for (size_t i = 0; i < tx->children_count; i++) {
        if (tx->children[i] == span) {
            tx->children[i] = tx->children[--tx->children_count];
            break;
        }
    }
    sentry__mutex_unlock(&tx->children_mutex);
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

    if (!sentry_value_decref(span->inner)) {
        sentry__transaction_remove_child(span->transaction, span);
        sentry__transaction_decref(span->transaction);
        sentry_free(span);
    }
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

    sentry__mutex_lock(&tx->children_mutex);
    if (tx->children_count == tx->children_cap) {
        size_t new_cap = tx->children_cap ? tx->children_cap * 2 : 4;
        sentry_span_t **new_children
            = sentry_malloc(new_cap * sizeof(sentry_span_t *));
        if (new_children) {
            if (tx->children) {
                memcpy(new_children, tx->children,
                    tx->children_count * sizeof(sentry_span_t *));
                sentry_free(tx->children);
            }
            tx->children = new_children;
            tx->children_cap = new_cap;
        } else {
            SENTRY_WARN("failed to track live span for crash auto-finalize");
        }
    }
    if (tx->children_count < tx->children_cap) {
        tx->children[tx->children_count++] = span;
    }
    sentry__mutex_unlock(&tx->children_mutex);

    return span;
}

sentry_value_t
sentry__value_span_new_n(size_t max_spans, sentry_value_t parent,
    sentry_slice_t operation, sentry_slice_t description, uint64_t timestamp)
{
    if (!sentry_value_is_null(sentry_value_get_by_key(parent, "timestamp"))) {
        SENTRY_WARN("span's parent is already finished, not creating span");
        goto fail;
    }

    sentry_value_t spans = sentry_value_get_by_key(parent, "spans");
    // This only checks that the number of _completed_ spans matches the
    // number of max spans. This means that the number of in-flight spans
    // can exceed the max number of spans.
    if (sentry_value_get_length(spans) >= max_spans) {
        SENTRY_WARN("reached maximum number of spans for transaction, not "
                    "creating span");
        goto fail;
    }

    sentry_value_t child = new_span_n(parent, operation);
    sentry_value_set_by_key(child, "description",
        sentry_value_new_string_n(description.ptr, description.len));
    sentry_value_set_by_key(child, "start_timestamp",
        sentry__value_new_string_owned(
            sentry__usec_time_to_iso8601(timestamp)));

    return child;
fail:
    return sentry_value_new_null();
}

sentry_value_t
sentry__value_span_new(size_t max_spans, sentry_value_t parent,
    const char *operation, const char *description, uint64_t timestamp)
{
    return sentry__value_span_new_n(max_spans, parent,
        sentry__slice_from_str(operation), sentry__slice_from_str(description),
        timestamp);
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

void
sentry_transaction_set_name_n(
    sentry_transaction_t *tx, const char *name, size_t name_len)
{
    if (tx) {
        sentry_value_set_by_key(tx->inner, "transaction",
            sentry_value_new_string_n(name, name_len));
    }
}

static void
set_tag_n(sentry_value_t item, sentry_slice_t tag, sentry_slice_t value)
{
    sentry_value_t tags = sentry_value_get_by_key(item, "tags");
    if (sentry_value_is_null(tags)) {
        tags = sentry_value_new_object();
        sentry_value_set_by_key(item, "tags", tags);
    }
    char *s = sentry__string_clone_max_n(value.ptr, value.len, 200);
    sentry_value_t tag_value
        = s ? sentry__value_new_string_owned(s) : sentry_value_new_null();
    sentry_value_set_by_key_n(tags, tag.ptr, tag.len, tag_value);
}

static void
set_tag(sentry_value_t item, const char *tag, const char *value)
{
    set_tag_n(item, sentry__slice_from_str(tag), sentry__slice_from_str(value));
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
sentry_transaction_set_tag_n(sentry_transaction_t *tx, const char *tag,
    size_t tag_len, const char *value, size_t value_len)
{
    if (tx) {
        set_tag_n(tx->inner, (sentry_slice_t) { tag, tag_len },
            (sentry_slice_t) { value, value_len });
    }
}

void
sentry_span_set_tag(sentry_span_t *span, const char *tag, const char *value)
{
    if (span) {
        set_tag(span->inner, tag, value);
    }
}

void
sentry_span_set_tag_n(sentry_span_t *span, const char *tag, size_t tag_len,
    const char *value, size_t value_len)
{
    if (span) {
        set_tag_n(span->inner, (sentry_slice_t) { tag, tag_len },
            (sentry_slice_t) { value, value_len });
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

static void
remove_tag_n(sentry_value_t item, const char *tag, size_t tag_len)
{
    sentry_value_t tags = sentry_value_get_by_key(item, "tags");
    if (!sentry_value_is_null(tags)) {
        sentry_value_remove_by_key_n(tags, tag, tag_len);
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
sentry_transaction_remove_tag_n(
    sentry_transaction_t *tx, const char *tag, size_t tag_len)
{
    if (tx) {
        remove_tag_n(tx->inner, tag, tag_len);
    }
}

void
sentry_span_remove_tag(sentry_span_t *span, const char *tag)
{
    if (span) {
        remove_tag(span->inner, tag);
    }
}

void
sentry_span_remove_tag_n(sentry_span_t *span, const char *tag, size_t tag_len)
{
    if (span) {
        remove_tag_n(span->inner, tag, tag_len);
    }
}

static void
set_data(sentry_value_t item, const char *data_key, size_t data_key_len,
    const char *key, size_t key_len, sentry_value_t value)
{
    sentry_value_t data
        = sentry_value_get_by_key_n(item, data_key, data_key_len);
    if (sentry_value_is_null(data)) {
        data = sentry_value_new_object();
        sentry_value_set_by_key_n(item, data_key, data_key_len, data);
    }
    sentry_value_set_by_key_n(data, key, key_len, value);
}

void
sentry_transaction_set_data(
    sentry_transaction_t *tx, const char *key, sentry_value_t value)
{
    if (key) {
        sentry_transaction_set_data_n(tx, key, strlen(key), value);
    }
}

static const char txn_data_key[] = "data";
static const size_t txn_data_key_len = sizeof(txn_data_key) - 1;

void
sentry_transaction_set_data_n(sentry_transaction_t *tx, const char *key,
    size_t key_len, sentry_value_t value)
{
    if (tx) {
        set_data(
            tx->inner, txn_data_key, txn_data_key_len, key, key_len, value);
    }
}

void
sentry_span_set_data(sentry_span_t *span, const char *key, sentry_value_t value)
{
    if (key) {
        sentry_span_set_data_n(span, key, strlen(key), value);
    }
}

static const char span_data_key[] = "data";
static const size_t span_data_key_len = sizeof(span_data_key) - 1;

void
sentry_span_set_data_n(
    sentry_span_t *span, const char *key, size_t key_len, sentry_value_t value)
{
    if (span) {
        set_data(
            span->inner, span_data_key, span_data_key_len, key, key_len, value);
    }
}

static void
remove_data(sentry_value_t item, const char *data_key, size_t data_key_len,
    const char *key, size_t key_len)
{
    sentry_value_t data
        = sentry_value_get_by_key_n(item, data_key, data_key_len);
    if (!sentry_value_is_null(data)) {
        sentry_value_remove_by_key_n(data, key, key_len);
    }
}

void
sentry_transaction_remove_data(sentry_transaction_t *tx, const char *key)
{
    if (key) {
        sentry_transaction_remove_data_n(tx, key, strlen(key));
    }
}

void
sentry_transaction_remove_data_n(
    sentry_transaction_t *tx, const char *key, size_t key_len)
{
    if (tx) {
        remove_data(tx->inner, txn_data_key, txn_data_key_len, key, key_len);
    }
}

void
sentry_span_remove_data(sentry_span_t *span, const char *key)
{
    if (key) {
        sentry_span_remove_data_n(span, key, strlen(key));
    }
}

void
sentry_span_remove_data_n(sentry_span_t *span, const char *key, size_t key_len)
{
    if (span) {
        remove_data(
            span->inner, span_data_key, span_data_key_len, key, key_len);
    }
}

static sentry_value_t
status_to_string(sentry_span_status_t status)
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

static void
set_status(sentry_value_t item, sentry_span_status_t status)
{
    sentry_value_set_by_key(item, "status", status_to_string(status));
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

    // (32 char trace_id)-(16-char span_id)-(0|1) + null terminator
    char buf[SENTRY_TRACE_LEN + 1];
    snprintf(buf, sizeof(buf), "%s-%s-%s", sentry_value_as_string(trace_id),
        sentry_value_as_string(span_id),
        sentry_value_is_true(sampled) ? "1" : "0");
    callback("sentry-trace", buf, userdata);

    // Outgoing baggage: build from the scope DSC (frozen from upstream when
    // the trace was continued, otherwise from the SDK's own options). The
    // span's own trace_id is preferred over any DSC trace_id to keep the
    // baggage trace_id consistent with the `sentry-trace` header above.
    // https://develop.sentry.dev/sdk/telemetry/traces/dynamic-sampling-context/#baggage-header
    {
        sentry_stringbuilder_t sb;
        sentry__stringbuilder_init(&sb);
        sentry__stringbuilder_append(&sb, "sentry-trace_id=");
        sentry__stringbuilder_append(&sb, sentry_value_as_string(trace_id));

        SENTRY_WITH_SCOPE (scope) {
            sentry__value_foreach_key_value(
                scope->dynamic_sampling_context, append_baggage_member, &sb);
        }

        char *baggage = sentry__stringbuilder_into_string(&sb);
        if (baggage) {
            callback("baggage", baggage, userdata);
            sentry_free(baggage);
        }
    }

    SENTRY_WITH_OPTIONS (options) {
        if (options->propagate_traceparent) {
            // 00-(32 char trace_id)-(16-char span_id)-(00|01) + null terminator
            char traceparent[SENTRY_W3C_TRACEPARENT_LEN + 1];
            snprintf(traceparent, sizeof(traceparent), "00-%s-%s-%s",
                sentry_value_as_string(trace_id),
                sentry_value_as_string(span_id),
                sentry_value_is_true(sampled) ? "01" : "00");
            // emit as lowercase as described on the W3C spec
            // https://www.w3.org/TR/trace-context/#header-name
            callback("traceparent", traceparent, userdata);
        }
    }
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

typedef struct {
    sentry_span_t *saved_span;
    sentry_transaction_t *saved_tx_obj;
    sentry_transaction_t *active_tx;
} saved_trace_t;

static saved_trace_t
save_active_trace(void)
{
    saved_trace_t s = { 0 };
    SENTRY_WITH_SCOPE (scope) {
        if (scope->span) {
            sentry__span_incref(scope->span);
            s.saved_span = scope->span;
        }
        if (scope->transaction_object) {
            sentry__transaction_incref(scope->transaction_object);
            s.saved_tx_obj = scope->transaction_object;
        }
    }
    s.active_tx = s.saved_span && s.saved_span->transaction
        ? s.saved_span->transaction
        : s.saved_tx_obj;
    if (s.active_tx) {
        sentry__transaction_incref(s.active_tx);
    }
    return s;
}

static void
restore_active_trace(saved_trace_t *s)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        if (!scope->span && s->saved_span) {
            scope->span = s->saved_span;
            s->saved_span = NULL;
        }
        if (!scope->transaction_object && s->saved_tx_obj) {
            scope->transaction_object = s->saved_tx_obj;
            s->saved_tx_obj = NULL;
        }
    }
    sentry__span_decref(s->saved_span);
    sentry__transaction_decref(s->saved_tx_obj);
}

// Atomically swap the live-children list off `tx` and finish each span.
// The swap ensures `sentry_span_finish_ts`'s per-span remove-scan is a no-op.
static void
finish_children(
    sentry_transaction_t *tx, sentry_span_status_t status, uint64_t end_ts)
{
    sentry__mutex_lock(&tx->children_mutex);
    sentry_span_t **children = tx->children;
    size_t count = tx->children_count;
    tx->children = NULL;
    tx->children_count = 0;
    tx->children_cap = 0;
    sentry__mutex_unlock(&tx->children_mutex);

    for (size_t i = count; i-- > 0;) {
        sentry_span_t *child = children[i];
        sentry__span_incref(child);
        sentry_span_set_status(child, status);
        sentry_span_finish_ts(child, end_ts);
    }
    sentry_free(children);
}

sentry_value_t
sentry__trace_finish(sentry_span_status_t status)
{
    // Save/restore scope around the finish so the crash event captured next
    // still inherits the active trace context (cf. sentry-cocoa's
    // `finishTracer:shouldCleanUp:NO`). Finished spans retain their ids; only
    // `timestamp` is added.
    saved_trace_t s = save_active_trace();
    if (!s.active_tx) {
        restore_active_trace(&s);
        return sentry_value_new_null();
    }

    uint64_t end_ts = sentry__usec_time();
    finish_children(s.active_tx, status, end_ts);

    sentry_transaction_set_status(s.active_tx, status);
    sentry_value_t tx = sentry__transaction_finish_value(s.active_tx, end_ts);

    restore_active_trace(&s);
    return tx;
}
