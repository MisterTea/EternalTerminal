#include "sentry_envelope.h"
#include "sentry_alloc.h"
#include "sentry_attachment.h"
#include "sentry_core.h"
#include "sentry_json.h"
#include "sentry_options.h"
#include "sentry_path.h"
#include "sentry_ratelimiter.h"
#include "sentry_scope.h"
#include "sentry_string.h"
#include "sentry_transport.h"
#include "sentry_uuid.h"
#include "sentry_value.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

struct sentry_envelope_item_s {
    sentry_value_t headers;
    sentry_value_t event;
    char *payload;
    size_t payload_len;
    sentry_envelope_item_t *next;
};

struct sentry_envelope_s {
    bool is_raw;
    union {
        struct {
            sentry_value_t headers;
            sentry_envelope_item_t *first_item;
            sentry_envelope_item_t *last_item;
            size_t item_count;
        } items;
        struct {
            char *payload;
            size_t payload_len;
        } raw;
    } contents;
};

static sentry_envelope_item_t *
envelope_add_item(sentry_envelope_t *envelope)
{
    if (envelope->is_raw) {
        return NULL;
    }

    // TODO: Envelopes may have at most one event item or one transaction item,
    // and not one of both. Some checking should be done here or in
    // `sentry__envelope_add_[transaction|event]` to ensure this can't happen.

    // Allocate new item
    sentry_envelope_item_t *item = SENTRY_MAKE(sentry_envelope_item_t);
    if (!item) {
        return NULL;
    }

    // Initialize item
    item->headers = sentry_value_new_object();
    item->event = sentry_value_new_null();
    item->payload = NULL;
    item->payload_len = 0;
    item->next = NULL;

    // Append to linked list
    if (envelope->contents.items.last_item) {
        envelope->contents.items.last_item->next = item;
    } else {
        envelope->contents.items.first_item = item;
    }
    envelope->contents.items.last_item = item;
    envelope->contents.items.item_count++;

    return item;
}

static void
envelope_item_cleanup(sentry_envelope_item_t *item)
{
    sentry_value_decref(item->headers);
    sentry_value_decref(item->event);
    sentry_free(item->payload);
}

static void
envelope_items_cleanup(sentry_envelope_t *envelope)
{
    sentry_value_decref(envelope->contents.items.headers);

    // Free all items in the linked list
    sentry_envelope_item_t *item = envelope->contents.items.first_item;
    while (item) {
        sentry_envelope_item_t *next = item->next;
        envelope_item_cleanup(item);
        sentry_free(item);
        item = next;
    }

    envelope->contents.items.first_item = NULL;
    envelope->contents.items.last_item = NULL;
    envelope->contents.items.item_count = 0;
}

sentry_value_t
sentry_envelope_get_header(const sentry_envelope_t *envelope, const char *key)
{
    return sentry_envelope_get_header_n(
        envelope, key, sentry__guarded_strlen(key));
}

sentry_value_t
sentry_envelope_get_header_n(
    const sentry_envelope_t *envelope, const char *key, size_t key_len)
{
    if (!envelope || envelope->is_raw) {
        return sentry_value_new_null();
    }
    return sentry_value_get_by_key_n(
        envelope->contents.items.headers, key, key_len);
}

void
sentry__envelope_item_set_header(
    sentry_envelope_item_t *item, const char *key, sentry_value_t value)
{
    sentry_value_set_by_key(item->headers, key, value);
}

static int
envelope_item_get_ratelimiter_category(const sentry_envelope_item_t *item)
{
    const char *ty = sentry_value_as_string(
        sentry_value_get_by_key(item->headers, "type"));
    if (sentry__string_eq(ty, "session")) {
        return SENTRY_RL_CATEGORY_SESSION;
    } else if (sentry__string_eq(ty, "transaction")) {
        return SENTRY_RL_CATEGORY_TRANSACTION;
    } else if (sentry__string_eq(ty, "replay_video")) {
        return SENTRY_RL_CATEGORY_REPLAY;
    } else if (sentry__string_eq(ty, "client_report")) {
        // internal telemetry, bypass rate limiting
        return -1;
    }
    // NOTE: the `type` here can be `event` or `attachment`.
    // Ideally, attachments should have their own RL_CATEGORY.
    return SENTRY_RL_CATEGORY_ERROR;
}

static sentry_data_category_t
item_type_to_data_category(const char *ty)
{
    if (sentry__string_eq(ty, "session")) {
        return SENTRY_DATA_CATEGORY_SESSION;
    } else if (sentry__string_eq(ty, "transaction")) {
        return SENTRY_DATA_CATEGORY_TRANSACTION;
    } else if (sentry__string_eq(ty, "attachment")) {
        return SENTRY_DATA_CATEGORY_ATTACHMENT;
    } else if (sentry__string_eq(ty, "log")) {
        return SENTRY_DATA_CATEGORY_LOG_ITEM;
    } else if (sentry__string_eq(ty, "feedback")) {
        return SENTRY_DATA_CATEGORY_FEEDBACK;
    } else if (sentry__string_eq(ty, "trace_metric")) {
        return SENTRY_DATA_CATEGORY_TRACE_METRIC;
    } else if (sentry__string_eq(ty, "replay_video")) {
        return SENTRY_DATA_CATEGORY_REPLAY;
    }
    return SENTRY_DATA_CATEGORY_ERROR;
}

bool
sentry__envelope_can_add_client_report(
    const sentry_envelope_t *envelope, const sentry_rate_limiter_t *rl)
{
    if (!envelope || envelope->is_raw) {
        return false;
    }
    for (const sentry_envelope_item_t *item
        = envelope->contents.items.first_item;
        item; item = item->next) {
        int category = envelope_item_get_ratelimiter_category(item);
        if (category < 0) {
            continue;
        }
        if (!rl || !sentry__rate_limiter_is_disabled(rl, category)) {
            return true;
        }
    }
    return false;
}

static sentry_envelope_item_t *
envelope_add_from_owned_buffer(
    sentry_envelope_t *envelope, char *buf, size_t buf_len, const char *type)
{
    if (!buf) {
        return NULL;
    }
    sentry_envelope_item_t *item = envelope_add_item(envelope);
    if (!item) {
        sentry_free(buf);
        return NULL;
    }

    item->payload = buf;
    item->payload_len = buf_len;
    sentry_value_t length = sentry_value_new_int32((int32_t)buf_len);
    sentry__envelope_item_set_header(
        item, "type", sentry_value_new_string(type));
    sentry__envelope_item_set_header(item, "length", length);

    return item;
}

void
sentry_envelope_free(sentry_envelope_t *envelope)
{
    if (!envelope) {
        return;
    }
    if (envelope->is_raw) {
        sentry_free(envelope->contents.raw.payload);
        sentry_free(envelope);
        return;
    }
    envelope_items_cleanup(envelope);
    sentry_free(envelope);
}

void
sentry__envelope_set_header(
    sentry_envelope_t *envelope, const char *key, sentry_value_t value)
{
    if (envelope->is_raw) {
        return;
    }
    sentry_value_set_by_key(envelope->contents.items.headers, key, value);
}

sentry_envelope_t *
sentry__envelope_new(void)
{
    sentry_dsn_t *dsn = NULL;
    SENTRY_WITH_OPTIONS (options) {
        dsn = sentry__dsn_incref(options->dsn);
    }
    sentry_envelope_t *rv = sentry__envelope_new_with_dsn(dsn);
    sentry__dsn_decref(dsn);
    return rv;
}

sentry_envelope_t *
sentry__envelope_new_with_dsn(const sentry_dsn_t *dsn)
{
    sentry_envelope_t *rv = SENTRY_MAKE(sentry_envelope_t);
    if (!rv) {
        return NULL;
    }

    rv->is_raw = false;
    rv->contents.items.first_item = NULL;
    rv->contents.items.last_item = NULL;
    rv->contents.items.item_count = 0;
    rv->contents.items.headers = sentry_value_new_object();

    if (dsn && dsn->is_valid) {
        sentry__envelope_set_header(
            rv, "dsn", sentry_value_new_string(dsn->raw));
    }

    return rv;
}

sentry_envelope_t *
sentry__envelope_from_path(const sentry_path_t *path)
{
    size_t buf_len;
    char *buf = sentry__path_read_to_buffer(path, &buf_len);
    if (!buf) {
        SENTRY_WARNF("failed to read raw envelope from \"%s\"", path->path);
        return NULL;
    }

    sentry_envelope_t *envelope = SENTRY_MAKE(sentry_envelope_t);
    if (!envelope) {
        sentry_free(buf);
        return NULL;
    }

    envelope->is_raw = true;
    envelope->contents.raw.payload = buf;
    envelope->contents.raw.payload_len = buf_len;

    return envelope;
}

sentry_uuid_t
sentry__envelope_get_event_id(const sentry_envelope_t *envelope)
{
    if (envelope->is_raw) {
        const char *payload = envelope->contents.raw.payload;
        size_t payload_len = envelope->contents.raw.payload_len;
        const char *newline = memchr(payload, '\n', payload_len);
        size_t header_len = newline ? (size_t)(newline - payload) : payload_len;
        sentry_value_t header = sentry__value_from_json(payload, header_len);
        sentry_uuid_t event_id = sentry_uuid_from_string(sentry_value_as_string(
            sentry_value_get_by_key(header, "event_id")));
        sentry_value_decref(header);
        return event_id;
    }
    return sentry_uuid_from_string(sentry_value_as_string(
        sentry_value_get_by_key(envelope->contents.items.headers, "event_id")));
}

void
sentry__envelope_set_event_id(
    sentry_envelope_t *envelope, const sentry_uuid_t *event_id)
{
    sentry_value_t value = sentry__value_new_uuid(event_id);
    sentry__envelope_set_header(envelope, "event_id", value);
}

sentry_value_t
sentry_envelope_get_event(const sentry_envelope_t *envelope)
{
    if (envelope->is_raw) {
        return sentry_value_new_null();
    }

    for (const sentry_envelope_item_t *item
        = envelope->contents.items.first_item;
        item; item = item->next) {
        if (!sentry_value_is_null(item->event)
            && !sentry__event_is_transaction(item->event)) {
            return item->event;
        }
    }
    return sentry_value_new_null();
}

sentry_value_t
sentry_envelope_get_transaction(const sentry_envelope_t *envelope)
{
    if (envelope->is_raw) {
        return sentry_value_new_null();
    }

    for (const sentry_envelope_item_t *item
        = envelope->contents.items.first_item;
        item; item = item->next) {
        if (!sentry_value_is_null(item->event)
            && sentry__event_is_transaction(item->event)) {
            return item->event;
        }
    }
    return sentry_value_new_null();
}

sentry_envelope_item_t *
sentry__envelope_add_event(sentry_envelope_t *envelope, sentry_value_t event)
{
    sentry_envelope_item_t *item = envelope_add_item(envelope);
    if (!item) {
        return NULL;
    }

    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_sb(NULL);
    if (!jw) {
        return NULL;
    }

    sentry_uuid_t event_id;
    sentry__ensure_event_id(event, &event_id);

    sentry__jsonwriter_write_value(jw, event);
    item->payload = sentry__jsonwriter_into_string(jw, &item->payload_len);
    if (!item->payload) {
        return NULL;
    }
    item->event = event;

    sentry__envelope_item_set_header(
        item, "type", sentry_value_new_string("event"));
    sentry_value_t length = sentry_value_new_int32((int32_t)item->payload_len);
    sentry__envelope_item_set_header(item, "length", length);

    sentry__envelope_set_event_id(envelope, &event_id);

    double traces_sample_rate = 0.0;
    SENTRY_WITH_OPTIONS (options) {
        traces_sample_rate = options->traces_sample_rate;
    }
    sentry_value_t dsc = sentry_value_new_null();
    sentry_value_t sample_rand = sentry_value_new_null();
    SENTRY_WITH_SCOPE (scope) {
        dsc = sentry__value_clone(scope->dynamic_sampling_context);
        sample_rand = sentry_value_get_by_key(
            sentry_value_get_by_key(scope->propagation_context, "trace"),
            "sample_rand");
    }
    if (!sentry_value_is_null(dsc)) {
        sentry_value_t trace_id = sentry_value_get_by_key(
            sentry_value_get_by_key(
                sentry_value_get_by_key(event, "contexts"), "trace"),
            "trace_id");
        if (!sentry_value_is_null(trace_id)) {
            sentry_value_incref(trace_id);
            sentry_value_set_by_key(dsc, "trace_id", trace_id);
        } else {
            SENTRY_WARN("couldn't retrieve trace_id from scope to apply to the "
                        "dynamic sampling context");
        }
        if (!sentry_value_is_null(sample_rand)) {
            if (sentry_value_as_double(sample_rand) >= traces_sample_rate) {
                sentry_value_set_by_key(
                    dsc, "sampled", sentry_value_new_string("false"));
            } else {
                sentry_value_set_by_key(
                    dsc, "sampled", sentry_value_new_string("true"));
            }
        } else {
            // only for testing; in production, the SDK should always have a
            // non-null sample_rand. We don't set "sampled" to keep dsc empty
            SENTRY_WARN("couldn't retrieve sample_rand from scope to apply to "
                        "the dynamic sampling context");
        }
        // only add dsc if it has values
        if (sentry_value_is_true(dsc)) {
#ifdef SENTRY_UNITTEST
            // to make comparing the header feasible in unit tests
            sentry_value_set_by_key(dsc, "sample_rand",
                sentry_value_new_double(0.01006918276309107));
#endif
            sentry__envelope_set_header(envelope, "trace", dsc);
        } else {
            sentry_value_decref(dsc);
        }
    }

    return item;
}

sentry_envelope_item_t *
sentry__envelope_add_transaction(
    sentry_envelope_t *envelope, sentry_value_t transaction)
{
    sentry_envelope_item_t *item = envelope_add_item(envelope);
    if (!item) {
        return NULL;
    }

    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_sb(NULL);
    if (!jw) {
        return NULL;
    }

    sentry_uuid_t event_id;
    sentry__ensure_event_id(transaction, &event_id);

    sentry__jsonwriter_write_value(jw, transaction);
    item->payload = sentry__jsonwriter_into_string(jw, &item->payload_len);
    if (!item->payload) {
        return NULL;
    }
    item->event = transaction;

    sentry__envelope_item_set_header(
        item, "type", sentry_value_new_string("transaction"));
    sentry_value_t length = sentry_value_new_int32((int32_t)item->payload_len);
    sentry__envelope_item_set_header(item, "length", length);

    sentry__envelope_set_event_id(envelope, &event_id);

    sentry_value_t dsc = sentry_value_new_null();

    SENTRY_WITH_SCOPE (scope) {
        dsc = sentry__value_clone(scope->dynamic_sampling_context);
    }

    if (!sentry_value_is_null(dsc)) {
        sentry_value_t trace_id = sentry_value_get_by_key(
            sentry_value_get_by_key(
                sentry_value_get_by_key(transaction, "contexts"), "trace"),
            "trace_id");
        if (!sentry_value_is_null(trace_id)) {
            sentry_value_incref(trace_id);
            sentry_value_set_by_key(dsc, "trace_id", trace_id);
            sentry_value_set_by_key(
                dsc, "sampled", sentry_value_new_string("true"));
        } else {
            SENTRY_WARN("couldn't retrieve trace_id in transaction's trace "
                        "context to apply to the dynamic sampling context");
        }
        sentry_value_t transaction_name
            = sentry_value_get_by_key(transaction, "transaction");
        if (!sentry_value_is_null(transaction_name)) {
            sentry_value_incref(transaction_name);
            sentry_value_set_by_key(dsc, "transaction", transaction_name);
        }
        // only add dsc if it has values
        if (sentry_value_is_true(dsc)) {
            sentry__envelope_set_header(envelope, "trace", dsc);
        } else {
            sentry_value_decref(dsc);
        }
    }

#ifdef SENTRY_UNITTEST
    sentry_value_t now = sentry_value_new_string("2021-12-16T05:53:59.343Z");
#else
    sentry_value_t now = sentry__value_new_string_owned(
        sentry__usec_time_to_iso8601(sentry__usec_time()));
#endif
    sentry__envelope_set_header(envelope, "sent_at", now);

    return item;
}

static sentry_envelope_item_t *
add_telemetry(sentry_envelope_t *envelope, sentry_value_t telemetry,
    const char *type, const char *content_type)
{
    sentry_envelope_item_t *item = envelope_add_item(envelope);
    if (!item) {
        return NULL;
    }

    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_sb(NULL);
    if (!jw) {
        return NULL;
    }

    sentry__jsonwriter_write_value(jw, telemetry);
    item->payload = sentry__jsonwriter_into_string(jw, &item->payload_len);
    if (!item->payload) {
        return NULL;
    }

    sentry__envelope_item_set_header(
        item, "type", sentry_value_new_string(type));
    sentry__envelope_item_set_header(item, "item_count",
        sentry_value_new_int32((int32_t)sentry_value_get_length(
            sentry_value_get_by_key(telemetry, "items"))));
    sentry__envelope_item_set_header(
        item, "content_type", sentry_value_new_string(content_type));
    sentry_value_t length = sentry_value_new_int32((int32_t)item->payload_len);
    sentry__envelope_item_set_header(item, "length", length);

    return item;
}

sentry_envelope_item_t *
sentry__envelope_add_logs(sentry_envelope_t *envelope, sentry_value_t logs)
{
    return add_telemetry(
        envelope, logs, "log", "application/vnd.sentry.items.log+json");
}

sentry_envelope_item_t *
sentry__envelope_add_metrics(
    sentry_envelope_t *envelope, sentry_value_t metrics)
{
    return add_telemetry(envelope, metrics, "trace_metric",
        "application/vnd.sentry.items.trace-metric+json");
}

sentry_envelope_item_t *
sentry__envelope_add_user_report(
    sentry_envelope_t *envelope, sentry_value_t user_report)
{
    sentry_envelope_item_t *item = envelope_add_item(envelope);
    if (!item) {
        return NULL;
    }

    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_sb(NULL);
    if (!jw) {
        return NULL;
    }

    sentry_uuid_t event_id;
    sentry__ensure_event_id(user_report, &event_id);

    sentry__jsonwriter_write_value(jw, user_report);
    item->payload = sentry__jsonwriter_into_string(jw, &item->payload_len);
    if (!item->payload) {
        return NULL;
    }

    sentry__envelope_item_set_header(
        item, "type", sentry_value_new_string("user_report"));
    sentry_value_t length = sentry_value_new_int32((int32_t)item->payload_len);
    sentry__envelope_item_set_header(item, "length", length);

    sentry__envelope_set_event_id(envelope, &event_id);

    return item;
}

sentry_envelope_item_t *
sentry__envelope_add_user_feedback(
    sentry_envelope_t *envelope, sentry_value_t user_feedback)
{
    sentry_value_t event = sentry_value_new_event();
    sentry_value_t contexts = sentry_value_get_by_key(event, "contexts");
    if (sentry_value_is_null(contexts)) {
        contexts = sentry_value_new_object();
    }
    sentry_value_set_by_key(contexts, "feedback", user_feedback);
    sentry_value_set_by_key(event, "contexts", contexts);

    sentry_envelope_item_t *item = sentry__envelope_add_event(envelope, event);
    if (!item) {
        sentry_value_decref(event);
        return NULL;
    }

    sentry__envelope_item_set_header(
        item, "type", sentry_value_new_string("feedback"));

    return item;
}

sentry_envelope_item_t *
sentry__envelope_add_session(
    sentry_envelope_t *envelope, const sentry_session_t *session)
{
    if (!envelope || !session) {
        return NULL;
    }
    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_sb(NULL);
    if (!jw) {
        return NULL;
    }
    sentry__session_to_json(session, jw);
    size_t payload_len = 0;
    char *payload = sentry__jsonwriter_into_string(jw, &payload_len);

    // NOTE: function will check for `payload` internally and free it on error
    return envelope_add_from_owned_buffer(
        envelope, payload, payload_len, "session");
}

static bool
attachment_ref_has_payload(const sentry_attachment_ref_t *ref)
{
    return ref && (ref->path || ref->location || ref->content_type);
}

static char *
attachment_ref_payload_to_string(
    const sentry_attachment_ref_t *ref, size_t *payload_len)
{
    *payload_len = 0;
    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_sb(NULL);
    if (!jw) {
        return NULL;
    }
    sentry_value_t obj = sentry_value_new_object();
    if (ref->path) {
        sentry_value_set_by_key(
            obj, "path", sentry_value_new_string(ref->path));
    }
    if (ref->location) {
        sentry_value_set_by_key(
            obj, "location", sentry_value_new_string(ref->location));
    }
    if (ref->content_type) {
        sentry_value_set_by_key(
            obj, "content_type", sentry_value_new_string(ref->content_type));
    }
    sentry__jsonwriter_write_value(jw, obj);
    sentry_value_decref(obj);
    return sentry__jsonwriter_into_string(jw, payload_len);
}

sentry_envelope_item_t *
sentry__envelope_add_attachment_ref(sentry_envelope_t *envelope,
    const sentry_attachment_ref_t *ref, const char *filename,
    const char *attachment_type, size_t attachment_length)
{
    if (!envelope) {
        return NULL;
    }
    if (envelope->is_raw) {
        return NULL;
    }
    size_t payload_len = 0;
    char *payload = NULL;
    if (attachment_ref_has_payload(ref)) {
        payload = attachment_ref_payload_to_string(ref, &payload_len);
        if (!payload) {
            return NULL;
        }
    }

    sentry_envelope_item_t *item = envelope_add_item(envelope);
    if (!item) {
        sentry_free(payload);
        return NULL;
    }
    sentry__envelope_item_set_header(
        item, "type", sentry_value_new_string("attachment"));
    sentry__envelope_item_set_header(item, "content_type",
        sentry_value_new_string(SENTRY_ATTACHMENT_REF_MIME));
    if (filename) {
        sentry__envelope_item_set_header(
            item, "filename", sentry_value_new_string(filename));
    }
    if (attachment_type && *attachment_type) {
        sentry__envelope_item_set_header(
            item, "attachment_type", sentry_value_new_string(attachment_type));
    }
    sentry__envelope_item_set_header(item, "attachment_length",
        sentry_value_new_uint64((uint64_t)attachment_length));

    if (payload) {
        item->payload = payload;
        item->payload_len = payload_len;
        sentry__envelope_item_set_header(
            item, "length", sentry_value_new_int32((int32_t)payload_len));
    }

    return item;
}

sentry_envelope_item_t *
sentry__envelope_add_attachment(
    sentry_envelope_t *envelope, const sentry_attachment_t *attachment)
{
    if (!envelope || !attachment) {
        return NULL;
    }

    sentry_envelope_item_t *item = NULL;
    if (attachment->buf) {
        item = sentry__envelope_add_from_buffer(
            envelope, attachment->buf, attachment->buf_len, "attachment");
    } else {
        item = sentry__envelope_add_from_path(
            envelope, attachment->path, "attachment");
    }
    if (!item) {
        return NULL;
    }
    if (attachment->type && *attachment->type) {
        sentry__envelope_item_set_header(
            item, "attachment_type", sentry_value_new_string(attachment->type));
    }
    if (attachment->content_type) {
        sentry__envelope_item_set_header(item, "content_type",
            sentry_value_new_string(attachment->content_type));
    }
    sentry__envelope_item_set_header(item, "filename",
        sentry_value_new_string(sentry__attachment_get_filename(attachment)));

    return item;
}

void
sentry__envelope_add_attachments(sentry_envelope_t *envelope,
    const sentry_attachment_t *attachments, const sentry_options_t *options)
{
    if (!envelope || !attachments) {
        return;
    }

    SENTRY_DEBUG("adding attachments to envelope");
    for (const sentry_attachment_t *attachment = attachments; attachment;
        attachment = attachment->next) {
        if (sentry__attachment_is_placeholder(attachment, options)) {
            continue;
        }
        sentry__envelope_add_attachment(envelope, attachment);
    }
}

sentry_envelope_item_t *
sentry__envelope_add_from_buffer(sentry_envelope_t *envelope, const char *buf,
    size_t buf_len, const char *type)
{
    // NOTE: function will check for the clone of `buf` internally and free it
    // on error
    return envelope_add_from_owned_buffer(
        envelope, sentry__string_clone_n(buf, buf_len), buf_len, type);
}

sentry_envelope_item_t *
sentry__envelope_add_from_path(
    sentry_envelope_t *envelope, const sentry_path_t *path, const char *type)
{
    if (!envelope) {
        return NULL;
    }
    size_t buf_len;
    char *buf = sentry__path_read_to_buffer(path, &buf_len);
    if (!buf) {
        SENTRY_WARNF("failed to read envelope item from \"%s\"", path->path);
        return NULL;
    }
    // NOTE: function will free `buf` on error
    return envelope_add_from_owned_buffer(envelope, buf, buf_len, type);
}

static void
sentry__envelope_serialize_headers_into_stringbuilder(
    const sentry_envelope_t *envelope, sentry_stringbuilder_t *sb)
{
    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_sb(sb);
    if (jw) {
        sentry__jsonwriter_write_value(jw, envelope->contents.items.headers);
        sentry__jsonwriter_free(jw);
    }
}

static void
sentry__envelope_serialize_item_into_stringbuilder(
    const sentry_envelope_item_t *item, sentry_stringbuilder_t *sb)
{
    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_sb(sb);
    if (!jw) {
        return;
    }
    sentry__stringbuilder_append_char(sb, '\n');

    sentry__jsonwriter_write_value(jw, item->headers);
    sentry__jsonwriter_free(jw);

    sentry__stringbuilder_append_char(sb, '\n');

    sentry__stringbuilder_append_buf(sb, item->payload, item->payload_len);
}

void
sentry__envelope_serialize_into_stringbuilder(
    const sentry_envelope_t *envelope, sentry_stringbuilder_t *sb)
{
    if (envelope->is_raw) {
        sentry__stringbuilder_append_buf(sb, envelope->contents.raw.payload,
            envelope->contents.raw.payload_len);
        return;
    }

    SENTRY_DEBUG("serializing envelope into buffer");
    sentry__envelope_serialize_headers_into_stringbuilder(envelope, sb);

    for (const sentry_envelope_item_t *item
        = envelope->contents.items.first_item;
        item; item = item->next) {
        sentry__envelope_serialize_item_into_stringbuilder(item, sb);
    }
}

char *
sentry_envelope_serialize_ratelimited(const sentry_envelope_t *envelope,
    const sentry_rate_limiter_t *rl, size_t *size_out, bool *owned_out)
{
    if (envelope->is_raw) {
        *size_out = envelope->contents.raw.payload_len;
        *owned_out = false;
        return envelope->contents.raw.payload;
    }
    *owned_out = true;

    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);
    sentry__envelope_serialize_headers_into_stringbuilder(envelope, &sb);

    size_t serialized_items = 0;
    for (const sentry_envelope_item_t *item
        = envelope->contents.items.first_item;
        item; item = item->next) {
        if (rl) {
            int category = envelope_item_get_ratelimiter_category(item);
            // category < 0 means the item should bypass rate limiting
            if (category >= 0
                && sentry__rate_limiter_is_disabled(rl, category)) {
                const char *ty = sentry_value_as_string(
                    sentry_value_get_by_key(item->headers, "type"));
                sentry__client_report_discard(
                    SENTRY_DISCARD_REASON_RATELIMIT_BACKOFF,
                    item_type_to_data_category(ty), 1);
                continue;
            }
        }
        sentry__envelope_serialize_item_into_stringbuilder(item, &sb);
        serialized_items += 1;
    }

    if (!serialized_items) {
        sentry__stringbuilder_cleanup(&sb);
        *size_out = 0;
        return NULL;
    }

    *size_out = sentry__stringbuilder_len(&sb);
    return sentry__stringbuilder_into_string(&sb);
}

char *
sentry_envelope_serialize(const sentry_envelope_t *envelope, size_t *size_out)
{
    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);

    sentry__envelope_serialize_into_stringbuilder(envelope, &sb);

    if (size_out) {
        *size_out = sentry__stringbuilder_len(&sb);
    }
    return sentry__stringbuilder_into_string(&sb);
}

/**
 * Called for each envelope item during serialization. Returns true if the
 * item was written externally and should be excluded from the envelope.
 */
typedef bool (*envelope_item_writer_fn)(
    const sentry_envelope_item_t *item, void *data);

static int
envelope_write_to_path(const sentry_envelope_t *envelope,
    const sentry_path_t *path, envelope_item_writer_fn item_writer, void *data)
{
    sentry_writer_t *output = sentry__writer_new_file(path);
    if (!output) {
        return 1;
    }

    int failed = 0;
    sentry_jsonwriter_t *jw = NULL;

    if (envelope->is_raw) {
        sentry__writer_write(output, envelope->contents.raw.payload,
            envelope->contents.raw.payload_len);
    } else {
        jw = sentry__jsonwriter_new_writer(output);
        if (!jw) {
            failed = 1;
            goto done;
        }

        sentry__jsonwriter_write_value(jw, envelope->contents.items.headers);
        sentry__jsonwriter_reset(jw);
        if (sentry__jsonwriter_has_failed(jw)) {
            goto done;
        }

        for (const sentry_envelope_item_t *item
            = envelope->contents.items.first_item;
            item; item = item->next) {
            if (item_writer && item_writer(item, data)) {
                continue;
            }

            sentry__writer_write_char(output, '\n');
            sentry__jsonwriter_write_value(jw, item->headers);
            sentry__jsonwriter_reset(jw);
            if (sentry__jsonwriter_has_failed(jw)) {
                goto done;
            }

            sentry__writer_write_char(output, '\n');
            sentry__writer_write(output, item->payload, item->payload_len);
            if (sentry__writer_has_failed(output)) {
                goto done;
            }
        }
    }

done:
    if (!sentry__writer_close(output)) {
        failed = 1;
    }
    if (jw && sentry__jsonwriter_has_failed(jw)) {
        failed = 1;
    }

    if (failed) {
        SENTRY_WARN("envelope write failed");
        // Best-effort cleanup for detected partial writes. This is not an
        // atomic publication guarantee; a process crash during writing can
        // still leave a partial file behind.
        sentry__path_remove(path);
    }
    sentry__jsonwriter_free(jw);
    sentry__writer_free(output);

    return failed;
}

MUST_USE int
sentry_envelope_write_to_path(
    const sentry_envelope_t *envelope, const sentry_path_t *path)
{
    return envelope_write_to_path(envelope, path, NULL, NULL);
}

int
sentry_envelope_write_to_file_n(
    const sentry_envelope_t *envelope, const char *path, size_t path_len)
{
    if (!envelope || !path) {
        return 1;
    }
    sentry_path_t *path_obj = sentry__path_from_str_n(path, path_len);

    int rv = sentry_envelope_write_to_path(envelope, path_obj);

    sentry__path_free(path_obj);

    return rv;
}

int
sentry_envelope_write_to_file(
    const sentry_envelope_t *envelope, const char *path)
{
    if (!envelope || !path) {
        return 1;
    }

    return sentry_envelope_write_to_file_n(envelope, path, strlen(path));
}

static bool deserialize_into(
    sentry_envelope_t *envelope, const char *buf, size_t buf_len);

sentry_envelope_t *
sentry_envelope_deserialize(const char *buf, size_t buf_len)
{
    // Use sentry__envelope_new_with_dsn(NULL) instead of sentry__envelope_new()
    // because the DSN is part of the serialized headers and will be restored by
    // deserialization. This avoids acquiring the options lock, which would
    // deadlock if called from a bgworker thread during shutdown.
    sentry_envelope_t *envelope = sentry__envelope_new_with_dsn(NULL);
    if (!envelope) {
        return NULL;
    }
    if (!deserialize_into(envelope, buf, buf_len)) {
        sentry_envelope_free(envelope);
        return NULL;
    }
    return envelope;
}

// https://develop.sentry.dev/sdk/data-model/envelopes/
static bool
deserialize_into(sentry_envelope_t *envelope, const char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return false;
    }

    const char *ptr = buf;
    const char *end = buf + buf_len;

    // headers
    const char *headers_end = memchr(ptr, '\n', (size_t)(end - ptr));
    if (!headers_end) {
        headers_end = end;
    }
    size_t headers_len = (size_t)(headers_end - ptr);
    sentry_value_decref(envelope->contents.items.headers);
    envelope->contents.items.headers
        = sentry__value_from_json(ptr, headers_len);
    if (sentry_value_get_type(envelope->contents.items.headers)
        != SENTRY_VALUE_TYPE_OBJECT) {
        return false;
    }

    ptr = headers_end;
    if (ptr < end) {
        ptr++; // skip newline
    }

    // items
    while (ptr < end) {
        sentry_envelope_item_t *item = envelope_add_item(envelope);
        if (!item) {
            return false;
        }

        // item headers
        const char *item_headers_end = memchr(ptr, '\n', (size_t)(end - ptr));
        if (!item_headers_end) {
            item_headers_end = end;
        }
        size_t item_headers_len = (size_t)(item_headers_end - ptr);
        sentry_value_decref(item->headers);
        item->headers = sentry__value_from_json(ptr, item_headers_len);
        if (sentry_value_get_type(item->headers) != SENTRY_VALUE_TYPE_OBJECT) {
            return false;
        }
        ptr = item_headers_end + 1; // skip newline

        if (ptr > end) {
            return false;
        }

        // item payload
        sentry_value_t length
            = sentry_value_get_by_key(item->headers, "length");
        if (sentry_value_is_null(length)) {
            // length omitted -> find newline or end of buffer
            const char *payload_end = memchr(ptr, '\n', (size_t)(end - ptr));
            if (!payload_end) {
                payload_end = end;
            }
            item->payload_len = (size_t)(payload_end - ptr);
        } else if (sentry_value_get_type(length) == SENTRY_VALUE_TYPE_UINT64) {
            uint64_t payload_len = sentry_value_as_uint64(length);
            if (payload_len >= SIZE_MAX) {
                return false;
            }
            item->payload_len = (size_t)payload_len;
        } else {
            int64_t payload_len = sentry_value_as_int64(length);
            if (payload_len < 0 || (uint64_t)payload_len >= SIZE_MAX) {
                return false;
            }
            item->payload_len = (size_t)payload_len;
        }
        if (item->payload_len > 0) {
            if (ptr + item->payload_len > end
                || item->payload_len >= SIZE_MAX) {
                return false;
            }
            item->payload = sentry_malloc(item->payload_len + 1);
            if (!item->payload) {
                return false;
            }
            memcpy(item->payload, ptr, item->payload_len);
            item->payload[item->payload_len] = '\0';

            // item event/transaction
            const char *type = sentry_value_as_string(
                sentry_value_get_by_key(item->headers, "type"));
            if (type
                && (sentry__string_eq(type, "event")
                    || sentry__string_eq(type, "transaction"))) {
                item->event
                    = sentry__value_from_json(item->payload, item->payload_len);
            }

            ptr += item->payload_len;
        }

        while (ptr < end && *ptr == '\n') {
            ptr++;
        }
    }

    return true;
}

static sentry_envelope_t *
parse_envelope_from_file(sentry_path_t *path)
{
    if (!path) {
        return NULL;
    }

    size_t buf_len = 0;
    char *buf = sentry__path_read_to_buffer(path, &buf_len);
    sentry_envelope_t *envelope = sentry_envelope_deserialize(buf, buf_len);
    sentry_free(buf);
    sentry__path_free(path);
    return envelope;
}

sentry_envelope_t *
sentry_envelope_read_from_file(const char *path)
{
    if (!path) {
        return NULL;
    }
    return parse_envelope_from_file(sentry__path_from_str(path));
}

sentry_envelope_t *
sentry_envelope_read_from_file_n(const char *path, size_t path_len)
{
    if (!path || path_len == 0) {
        return NULL;
    }
    return parse_envelope_from_file(sentry__path_from_str_n(path, path_len));
}

#ifdef SENTRY_PLATFORM_WINDOWS
sentry_envelope_t *
sentry_envelope_read_from_filew(const wchar_t *path)
{
    if (!path) {
        return NULL;
    }
    return parse_envelope_from_file(sentry__path_from_wstr(path));
}

sentry_envelope_t *
sentry_envelope_read_from_filew_n(const wchar_t *path, size_t path_len)
{
    if (!path || path_len == 0) {
        return NULL;
    }
    return parse_envelope_from_file(sentry__path_from_wstr_n(path, path_len));
}
#endif

static sentry_envelope_t *
envelope_materialize(const sentry_envelope_t *envelope)
{
    if (!envelope || !envelope->is_raw) {
        return (sentry_envelope_t *)envelope;
    }
    return sentry_envelope_deserialize(
        envelope->contents.raw.payload, envelope->contents.raw.payload_len);
}

typedef struct {
    const sentry_path_t *dir;
    const sentry_uuid_t *event_id;
    int index;
} minidump_writer_t;

static bool
write_minidump(const sentry_envelope_item_t *item, void *data)
{
    const char *att_type = sentry_value_as_string(
        sentry_value_get_by_key(item->headers, "attachment_type"));
    const char *content_type = sentry_value_as_string(
        sentry_value_get_by_key(item->headers, "content_type"));
    if (!sentry__string_eq(att_type, "event.minidump") || !item->payload
        || item->payload_len == 0
        || (content_type
            && sentry__string_eq(content_type, SENTRY_ATTACHMENT_REF_MIME))) {
        return false;
    }

    minidump_writer_t *ctx = data;

    char suffix[16];
    if (ctx->index == 0) {
        memcpy(suffix, ".dmp", 5);
    } else {
        snprintf(suffix, sizeof(suffix), "-%d.dmp", ctx->index);
    }

    char *filename = sentry__uuid_as_filename(ctx->event_id, suffix);
    if (!filename) {
        return false;
    }
    sentry_path_t *path = sentry__path_join_str(ctx->dir, filename);
    sentry_free(filename);
    if (!path) {
        return false;
    }

    int rv = sentry__path_write_buffer(path, item->payload, item->payload_len);
    if (rv != 0) {
        SENTRY_WARNF("failed to write minidump to \"%s\"", path->path);
    } else {
        ctx->index++;
    }
    sentry__path_free(path);
    return rv == 0;
}

int
sentry__envelope_write_to_cache(
    const sentry_envelope_t *envelope, const sentry_path_t *cache_dir)
{
    if (!envelope || !cache_dir) {
        return 1;
    }

    sentry_uuid_t event_id = sentry__envelope_get_event_id(envelope);
    if (sentry_uuid_is_nil(&event_id)) {
        event_id = sentry_uuid_new_v4();
    }

    char *filename = sentry__uuid_as_filename(&event_id, ".envelope");
    if (!filename) {
        return 1;
    }
    sentry_path_t *path = sentry__path_join_str(cache_dir, filename);
    sentry_free(filename);
    if (!path) {
        return 1;
    }

    // already cached
    if (sentry__path_is_file(path)) {
        sentry__path_free(path);
        return 0;
    }

    sentry_envelope_t *materialized = envelope_materialize(envelope);
    if (!materialized) {
        sentry__path_free(path);
        return 1;
    }

    minidump_writer_t ctx = { cache_dir, &event_id, 0 };

    int rv = envelope_write_to_path(materialized, path, write_minidump, &ctx);
    sentry__path_free(path);
    if (materialized != envelope) {
        sentry_envelope_free(materialized);
    }
    return rv;
}

static const char *
discard_reason_to_string(sentry_discard_reason_t reason)
{
    switch (reason) {
    case SENTRY_DISCARD_REASON_QUEUE_OVERFLOW:
        return "queue_overflow";
    case SENTRY_DISCARD_REASON_RATELIMIT_BACKOFF:
        return "ratelimit_backoff";
    case SENTRY_DISCARD_REASON_NETWORK_ERROR:
        return "network_error";
    case SENTRY_DISCARD_REASON_SAMPLE_RATE:
        return "sample_rate";
    case SENTRY_DISCARD_REASON_BEFORE_SEND:
        return "before_send";
    case SENTRY_DISCARD_REASON_SEND_ERROR:
        return "send_error";
    case SENTRY_DISCARD_REASON_MAX:
    default:
        return "unknown";
    }
}

static const char *
data_category_to_string(sentry_data_category_t category)
{
    switch (category) {
    case SENTRY_DATA_CATEGORY_ERROR:
        return "error";
    case SENTRY_DATA_CATEGORY_SESSION:
        return "session";
    case SENTRY_DATA_CATEGORY_TRANSACTION:
        return "transaction";
    case SENTRY_DATA_CATEGORY_ATTACHMENT:
        return "attachment";
    case SENTRY_DATA_CATEGORY_LOG_ITEM:
        return "log_item";
    case SENTRY_DATA_CATEGORY_FEEDBACK:
        return "feedback";
    case SENTRY_DATA_CATEGORY_TRACE_METRIC:
        return "trace_metric";
    case SENTRY_DATA_CATEGORY_REPLAY:
        return "replay";
    case SENTRY_DATA_CATEGORY_MAX:
    default:
        return "unknown";
    }
}

sentry_envelope_item_t *
sentry__envelope_add_client_report(
    sentry_envelope_t *envelope, const sentry_client_report_t *report)
{
    if (!envelope || envelope->is_raw || !report) {
        return NULL;
    }

    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_sb(NULL);
    if (!jw) {
        return NULL;
    }

    sentry__jsonwriter_write_object_start(jw);
    sentry__jsonwriter_write_key(jw, "timestamp");
    char *timestamp = sentry__usec_time_to_iso8601(sentry__usec_time());
    sentry__jsonwriter_write_str(jw, timestamp);
    sentry_free(timestamp);

    sentry__jsonwriter_write_key(jw, "discarded_events");
    sentry__jsonwriter_write_list_start(jw);
    for (int r = 0; r < SENTRY_DISCARD_REASON_MAX; r++) {
        for (int c = 0; c < SENTRY_DATA_CATEGORY_MAX; c++) {
            if (report->counts[r][c] > 0) {
                sentry__jsonwriter_write_object_start(jw);
                sentry__jsonwriter_write_key(jw, "reason");
                sentry__jsonwriter_write_str(jw, discard_reason_to_string(r));
                sentry__jsonwriter_write_key(jw, "category");
                sentry__jsonwriter_write_str(jw, data_category_to_string(c));
                sentry__jsonwriter_write_key(jw, "quantity");
                sentry__jsonwriter_write_int64(jw, report->counts[r][c]);
                sentry__jsonwriter_write_object_end(jw);
            }
        }
    }
    sentry__jsonwriter_write_list_end(jw);
    sentry__jsonwriter_write_object_end(jw);

    size_t payload_len = 0;
    char *payload = sentry__jsonwriter_into_string(jw, &payload_len);
    if (!payload) {
        return NULL;
    }

    sentry_envelope_item_t *item = sentry__envelope_add_from_buffer(
        envelope, payload, payload_len, "client_report");
    sentry_free(payload);

    return item;
}

void
sentry__envelope_discard(const sentry_envelope_t *envelope,
    sentry_discard_reason_t reason, const sentry_rate_limiter_t *rl)
{
    if (envelope->is_raw) {
        return;
    }
    for (const sentry_envelope_item_t *item
        = envelope->contents.items.first_item;
        item; item = item->next) {
        int category = envelope_item_get_ratelimiter_category(item);
        // internal items (e.g. client_report) bypass rate limiting
        if (category < 0) {
            continue;
        }
        // already recorded as ratelimit_backoff
        if (rl && sentry__rate_limiter_is_disabled(rl, category)) {
            continue;
        }
        const char *ty = sentry_value_as_string(
            sentry_value_get_by_key(item->headers, "type"));
        sentry__client_report_discard(
            reason, item_type_to_data_category(ty), 1);
    }
}

bool
sentry__envelope_is_raw(const sentry_envelope_t *envelope)
{
    return envelope && envelope->is_raw;
}

size_t
sentry__envelope_get_item_count(const sentry_envelope_t *envelope)
{
    return envelope->is_raw ? 0 : envelope->contents.items.item_count;
}

sentry_envelope_item_t *
sentry__envelope_get_item(const sentry_envelope_t *envelope, size_t idx)
{
    if (envelope->is_raw) {
        return NULL;
    }

    // Traverse linked list to find item at index
    size_t current_idx = 0;
    for (sentry_envelope_item_t *item = envelope->contents.items.first_item;
        item; item = item->next) {
        if (current_idx == idx) {
            return item;
        }
        current_idx++;
    }

    return NULL;
}

bool
sentry__envelope_remove_item(
    sentry_envelope_t *envelope, sentry_envelope_item_t *item)
{
    if (!envelope || envelope->is_raw || !item) {
        return false;
    }

    sentry_envelope_item_t *prev = NULL;
    sentry_envelope_item_t *cur = envelope->contents.items.first_item;
    while (cur) {
        if (cur == item) {
            if (prev) {
                prev->next = cur->next;
            } else {
                envelope->contents.items.first_item = cur->next;
            }
            if (envelope->contents.items.last_item == cur) {
                envelope->contents.items.last_item = prev;
            }
            envelope->contents.items.item_count--;
            envelope_item_cleanup(cur);
            sentry_free(cur);
            return true;
        }
        prev = cur;
        cur = cur->next;
    }

    return false;
}

// these for now are only needed for tests
#ifdef SENTRY_UNITTEST
sentry_value_t
sentry__envelope_item_get_header(
    const sentry_envelope_item_t *item, const char *key)
{
    return sentry_value_get_by_key(item->headers, key);
}

const char *
sentry__envelope_item_get_payload(
    const sentry_envelope_item_t *item, size_t *payload_len_out)
{
    if (payload_len_out) {
        *payload_len_out = item->payload_len;
    }
    return item->payload;
}
#endif

bool
sentry__envelope_materialize(sentry_envelope_t *envelope)
{
    if (!envelope) {
        return false;
    }
    if (!envelope->is_raw) {
        return true;
    }
    char *payload = envelope->contents.raw.payload;
    size_t payload_len = envelope->contents.raw.payload_len;
    envelope->is_raw = false;
    envelope->contents.items.headers = sentry_value_new_object();
    envelope->contents.items.first_item = NULL;
    envelope->contents.items.last_item = NULL;
    envelope->contents.items.item_count = 0;
    bool ok = deserialize_into(envelope, payload, payload_len);
    if (!ok) {
        envelope_items_cleanup(envelope);
        envelope->is_raw = true;
        envelope->contents.raw.payload = payload;
        envelope->contents.raw.payload_len = payload_len;
        return false;
    }

    sentry_free(payload);
    return true;
}

static bool
sentry__envelope_item_is_attachment_ref(const sentry_envelope_item_t *item)
{
    if (!item) {
        return false;
    }
    const char *ct = sentry_value_as_string(
        sentry_value_get_by_key(item->headers, "content_type"));
    return ct && strcmp(ct, SENTRY_ATTACHMENT_REF_MIME) == 0;
}

bool
sentry__envelope_item_get_attachment_ref(
    const sentry_envelope_item_t *item, sentry_attachment_ref_t *ref)
{
    if (!ref) {
        return false;
    }
    memset(ref, 0, sizeof(*ref));
    if (!sentry__envelope_item_is_attachment_ref(item)) {
        return false;
    }
    if (!item->payload) {
        ref->_owner = sentry_value_new_null();
        return true;
    }
    ref->_owner = sentry__value_from_json(item->payload, item->payload_len);
    ref->path
        = sentry_value_as_string(sentry_value_get_by_key(ref->_owner, "path"));
    ref->location = sentry_value_as_string(
        sentry_value_get_by_key(ref->_owner, "location"));
    ref->content_type = sentry_value_as_string(
        sentry_value_get_by_key(ref->_owner, "content_type"));
    ref->attachment_type = sentry_value_as_string(
        sentry_value_get_by_key(item->headers, "attachment_type"));
    return true;
}

void
sentry__attachment_ref_cleanup(sentry_attachment_ref_t *ref)
{
    if (!ref) {
        return;
    }
    sentry_value_decref(ref->_owner);
    memset(ref, 0, sizeof(*ref));
}

static bool
set_attachment_ref_payload(
    sentry_envelope_item_t *item, const sentry_attachment_ref_t *ref)
{
    if (!item || !attachment_ref_has_payload(ref)) {
        return false;
    }
    size_t payload_len = 0;
    char *payload = attachment_ref_payload_to_string(ref, &payload_len);
    if (!payload) {
        return false;
    }
    sentry_free(item->payload);
    item->payload = payload;
    item->payload_len = payload_len;
    sentry__envelope_item_set_header(
        item, "length", sentry_value_new_int32((int32_t)payload_len));
    return true;
}

bool
sentry__envelope_item_resolve_attachment_ref(
    sentry_envelope_item_t *item, const char *location)
{
    if (!sentry__guarded_strlen(location)) {
        return false;
    }
    sentry_attachment_ref_t ref;
    if (!sentry__envelope_item_get_attachment_ref(item, &ref)) {
        return false;
    }
    sentry_attachment_ref_t resolved = { 0 };
    if (sentry__guarded_strlen(ref.path)) {
        resolved.path = ref.path;
    }
    if (sentry__guarded_strlen(ref.attachment_type)) {
        resolved.attachment_type = ref.attachment_type;
    }
    resolved.location = location;
    if (sentry__guarded_strlen(ref.content_type)) {
        resolved.content_type = ref.content_type;
    }
    bool ok = set_attachment_ref_payload(item, &resolved);
    sentry__attachment_ref_cleanup(&ref);
    return ok;
}

bool
sentry__envelope_has_content_type(
    const sentry_envelope_t *envelope, const char *content_type)
{
    if (!envelope || !content_type) {
        return false;
    }
    if (envelope->is_raw) {
        size_t ct_len = strlen(content_type);
        const char *p = envelope->contents.raw.payload;
        size_t len = envelope->contents.raw.payload_len;
        if (len < ct_len || ct_len == 0) {
            return false;
        }
        const char *end = p + len - ct_len;
        while (p <= end) {
            if (*p == content_type[0] && memcmp(p, content_type, ct_len) == 0) {
                return true;
            }
            p++;
        }
        return false;
    }
    for (const sentry_envelope_item_t *item
        = envelope->contents.items.first_item;
        item; item = item->next) {
        const char *ct = sentry_value_as_string(
            sentry_value_get_by_key(item->headers, "content_type"));
        if (ct && strcmp(ct, content_type) == 0) {
            return true;
        }
    }
    return false;
}
