#ifndef SENTRY_ENVELOPE_H_INCLUDED
#define SENTRY_ENVELOPE_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_core.h"

#include "sentry_path.h"
#include "sentry_session.h"
#include "sentry_string.h"

#define SENTRY_MAX_ENVELOPE_ITEMS 10

typedef struct sentry_envelope_item_s sentry_envelope_item_t;
typedef struct sentry_rate_limiter_s sentry_rate_limiter_t;

/**
 * Create a new empty envelope.
 */
sentry_envelope_t *sentry__envelope_new(void);

/**
 * This loads a previously serialized envelope from disk.
 */
sentry_envelope_t *sentry__envelope_from_path(const sentry_path_t *path);

/**
 * This returns the UUID of the event associated with this envelope.
 * If there is no event inside this envelope, or the envelope was previously
 * loaded from disk, the empty nil UUID will be returned.
 */
sentry_uuid_t sentry__envelope_get_event_id(const sentry_envelope_t *envelope);

/**
 * Add an event to this envelope.
 */
sentry_envelope_item_t *sentry__envelope_add_event(
    sentry_envelope_t *envelope, sentry_value_t event);

/**
 * Add a session to this envelope.
 */
sentry_envelope_item_t *sentry__envelope_add_session(
    sentry_envelope_t *envelope, const sentry_session_t *session);

/**
 * This will add the file contents from `path` as an envelope item of type
 * `type`.
 */
sentry_envelope_item_t *sentry__envelope_add_from_path(
    sentry_envelope_t *envelope, const sentry_path_t *path, const char *type);

/**
 * This will add the given buffer as a new envelope item of type `type`.
 */
sentry_envelope_item_t *sentry__envelope_add_from_buffer(
    sentry_envelope_t *envelope, const char *buf, size_t buf_len,
    const char *type);

/**
 * This sets an explicit header for the given envelope item.
 */
void sentry__envelope_item_set_header(
    sentry_envelope_item_t *item, const char *key, sentry_value_t value);

/**
 * Serialize the envelope while applying the rate limits from `rl`.
 * Returns `NULL` when all items have been rate-limited, and might return a
 * pointer to borrowed data in case of a raw envelope, in which case `owned_out`
 * will be set to `false`.
 */
char *sentry_envelope_serialize_ratelimited(const sentry_envelope_t *envelope,
    const sentry_rate_limiter_t *rl, size_t *size_out, bool *owned_out);

/**
 * Serialize a complete envelope with all its items into the given string
 * builder.
 */
void sentry__envelope_serialize_into_stringbuilder(
    const sentry_envelope_t *envelope, sentry_stringbuilder_t *sb);

/**
 * Serialize the envelope, and write it to a new file at `path`.
 * The envelope can later be loaded using `sentry__envelope_from_path`.
 */
MUST_USE int sentry_envelope_write_to_path(
    const sentry_envelope_t *envelope, const sentry_path_t *path);

// these for now are only needed for tests
#if SENTRY_UNITTEST
size_t sentry__envelope_get_item_count(const sentry_envelope_t *envelope);
const sentry_envelope_item_t *sentry__envelope_get_item(
    const sentry_envelope_t *envelope, size_t idx);
sentry_value_t sentry__envelope_item_get_header(
    const sentry_envelope_item_t *item, const char *key);
const char *sentry__envelope_item_get_payload(
    const sentry_envelope_item_t *item, size_t *payload_len_out);
#endif

#endif
