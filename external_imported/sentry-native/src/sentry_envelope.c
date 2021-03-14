#include "sentry_envelope.h"
#include "sentry_alloc.h"
#include "sentry_core.h"
#include "sentry_json.h"
#include "sentry_options.h"
#include "sentry_path.h"
#include "sentry_ratelimiter.h"
#include "sentry_string.h"
#include "sentry_transport.h"
#include "sentry_value.h"
#include <string.h>

struct sentry_envelope_item_s {
    sentry_value_t headers;
    sentry_value_t event;
    char *payload;
    size_t payload_len;
};

struct sentry_envelope_s {
    bool is_raw;
    union {
        struct {
            sentry_value_t headers;
            sentry_envelope_item_t items[SENTRY_MAX_ENVELOPE_ITEMS];
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
    if (envelope->contents.items.item_count >= SENTRY_MAX_ENVELOPE_ITEMS) {
        return NULL;
    }

    sentry_envelope_item_t *rv
        = &envelope->contents.items
               .items[envelope->contents.items.item_count++];
    rv->headers = sentry_value_new_object();
    rv->event = sentry_value_new_null();
    rv->payload = NULL;
    rv->payload_len = 0;
    return rv;
}

static void
envelope_item_cleanup(sentry_envelope_item_t *item)
{
    sentry_value_decref(item->headers);
    sentry_value_decref(item->event);
    sentry_free(item->payload);
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
    }
    // NOTE: the `type` here can be `event` or `attachment`.
    // Ideally, attachments should have their own RL_CATEGORY.
    return SENTRY_RL_CATEGORY_ERROR;
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
    sentry_value_decref(envelope->contents.items.headers);
    for (size_t i = 0; i < envelope->contents.items.item_count; i++) {
        envelope_item_cleanup(&envelope->contents.items.items[i]);
    }
    sentry_free(envelope);
}

static void
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
    sentry_envelope_t *rv = SENTRY_MAKE(sentry_envelope_t);
    if (!rv) {
        return NULL;
    }

    rv->is_raw = false;
    rv->contents.items.item_count = 0;
    rv->contents.items.headers = sentry_value_new_object();

    SENTRY_WITH_OPTIONS (options) {
        if (options->dsn && options->dsn->is_valid) {
            sentry__envelope_set_header(rv, "dsn",
                sentry_value_new_string(sentry_options_get_dsn(options)));
        }
    }

    return rv;
}

sentry_envelope_t *
sentry__envelope_from_path(const sentry_path_t *path)
{
    size_t buf_len;
    char *buf = sentry__path_read_to_buffer(path, &buf_len);
    if (!buf) {
        SENTRY_WARNF("failed to read raw envelope from \"%" SENTRY_PATH_PRI
                     "\"",
            path->path);
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
        return sentry_uuid_nil();
    }
    return sentry_uuid_from_string(sentry_value_as_string(
        sentry_value_get_by_key(envelope->contents.items.headers, "event_id")));
}

sentry_value_t
sentry_envelope_get_event(const sentry_envelope_t *envelope)
{
    if (envelope->is_raw) {
        return sentry_value_new_null();
    }
    for (size_t i = 0; i < envelope->contents.items.item_count; i++) {
        if (!sentry_value_is_null(envelope->contents.items.items[i].event)) {
            return envelope->contents.items.items[i].event;
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

    sentry_value_t event_id = sentry__ensure_event_id(event, NULL);

    item->event = event;
    item->payload = sentry_value_to_json(event);
    item->payload_len = strlen(item->payload);
    sentry__envelope_item_set_header(
        item, "type", sentry_value_new_string("event"));
    sentry_value_t length = sentry_value_new_int32((int32_t)item->payload_len);
    sentry__envelope_item_set_header(item, "length", length);

    sentry_value_incref(event_id);
    sentry__envelope_set_header(envelope, "event_id", event_id);

    return item;
}

sentry_envelope_item_t *
sentry__envelope_add_session(
    sentry_envelope_t *envelope, const sentry_session_t *session)
{
    if (!envelope || !session) {
        return NULL;
    }
    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_in_memory();
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

sentry_envelope_item_t *
sentry__envelope_add_from_buffer(sentry_envelope_t *envelope, const char *buf,
    size_t buf_len, const char *type)
{
    // NOTE: function will check for the clone of `buf` internally and free it
    // on error
    return envelope_add_from_owned_buffer(
        envelope, sentry__string_clonen(buf, buf_len), buf_len, type);
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
        SENTRY_WARNF("failed to read envelope item from \"%" SENTRY_PATH_PRI
                     "\"",
            path->path);
        return NULL;
    }
    // NOTE: function will free `buf` on error
    return envelope_add_from_owned_buffer(envelope, buf, buf_len, type);
}

static void
sentry__envelope_serialize_headers_into_stringbuilder(
    const sentry_envelope_t *envelope, sentry_stringbuilder_t *sb)
{
    char *buf = sentry_value_to_json(envelope->contents.items.headers);
    sentry__stringbuilder_append(sb, buf);
    sentry_free(buf);
}

static void
sentry__envelope_serialize_item_into_stringbuilder(
    const sentry_envelope_item_t *item, sentry_stringbuilder_t *sb)
{
    sentry__stringbuilder_append_char(sb, '\n');
    char *buf = sentry_value_to_json(item->headers);
    sentry__stringbuilder_append(sb, buf);
    sentry__stringbuilder_append_char(sb, '\n');
    sentry_free(buf);

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

    SENTRY_TRACE("serializing envelope into buffer");
    sentry__envelope_serialize_headers_into_stringbuilder(envelope, sb);

    for (size_t i = 0; i < envelope->contents.items.item_count; i++) {
        const sentry_envelope_item_t *item = &envelope->contents.items.items[i];
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
    for (size_t i = 0; i < envelope->contents.items.item_count; i++) {
        const sentry_envelope_item_t *item = &envelope->contents.items.items[i];
        if (rl) {
            int category = envelope_item_get_ratelimiter_category(item);
            if (sentry__rate_limiter_is_disabled(rl, category)) {
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

    *size_out = sentry__stringbuilder_len(&sb);
    return sentry__stringbuilder_into_string(&sb);
}

MUST_USE int
sentry_envelope_write_to_path(
    const sentry_envelope_t *envelope, const sentry_path_t *path)
{
    // TODO: This currently builds the whole buffer in-memory.
    // It would be nice to actually stream this to a file.
    size_t buf_len = 0;
    char *buf = sentry_envelope_serialize(envelope, &buf_len);

    int rv = sentry__path_write_buffer(path, buf, buf_len);

    sentry_free(buf);

    return rv;
}

int
sentry_envelope_write_to_file(
    const sentry_envelope_t *envelope, const char *path)
{
    sentry_path_t *path_obj = sentry__path_from_str(path);

    int rv = sentry_envelope_write_to_path(envelope, path_obj);

    sentry__path_free(path_obj);

    return rv;
}

#if SENTRY_UNITTEST
size_t
sentry__envelope_get_item_count(const sentry_envelope_t *envelope)
{
    return envelope->is_raw ? 0 : envelope->contents.items.item_count;
}

const sentry_envelope_item_t *
sentry__envelope_get_item(const sentry_envelope_t *envelope, size_t idx)
{
    return !envelope->is_raw && idx < envelope->contents.items.item_count
        ? &envelope->contents.items.items[idx]
        : NULL;
}

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
