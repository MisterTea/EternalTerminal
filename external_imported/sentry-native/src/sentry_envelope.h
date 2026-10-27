#ifndef SENTRY_ENVELOPE_H_INCLUDED
#define SENTRY_ENVELOPE_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_core.h"

#include "sentry_attachment.h"
#include "sentry_client_report.h"
#include "sentry_path.h"
#include "sentry_ratelimiter.h"
#include "sentry_session.h"
#include "sentry_string.h"
#include "sentry_utils.h"

// https://develop.sentry.dev/sdk/data-model/envelopes/#size-limits
#define SENTRY_MAX_ENVELOPE_SESSIONS 100
#define SENTRY_ATTACHMENT_REF_MIME "application/vnd.sentry.attachment-ref+json"

typedef struct sentry_envelope_item_s sentry_envelope_item_t;

typedef struct {
    const char *path;
    const char *location;
    const char *content_type;
    const char *attachment_type;
    sentry_value_t _owner;
} sentry_attachment_ref_t;

/**
 * Create a new empty envelope.
 */
sentry_envelope_t *sentry__envelope_new(void);

/**
 * Create a new empty envelope with the given DSN header.
 */
sentry_envelope_t *sentry__envelope_new_with_dsn(const sentry_dsn_t *dsn);

/**
 * This loads a previously serialized envelope from disk.
 */
sentry_envelope_t *sentry__envelope_from_path(const sentry_path_t *path);

/**
 * This returns the UUID of the event associated with this envelope.
 * If there is no event inside this envelope, the empty nil UUID will be
 * returned.
 */
sentry_uuid_t sentry__envelope_get_event_id(const sentry_envelope_t *envelope);

/**
 * Set the event ID header for this envelope.
 */
void sentry__envelope_set_event_id(
    sentry_envelope_t *envelope, const sentry_uuid_t *event_id);

/**
 * Add an event to this envelope.
 */
sentry_envelope_item_t *sentry__envelope_add_event(
    sentry_envelope_t *envelope, sentry_value_t event);

/**
 * Add a transaction to this envelope.
 */
sentry_envelope_item_t *sentry__envelope_add_transaction(
    sentry_envelope_t *envelope, sentry_value_t transaction);

/**
 * Add a deprecated user report to this envelope.
 */
sentry_envelope_item_t *sentry__envelope_add_user_report(
    sentry_envelope_t *envelope, sentry_value_t user_report);

/**
 * Add a list of logs to this envelope.
 */
sentry_envelope_item_t *sentry__envelope_add_logs(
    sentry_envelope_t *envelope, sentry_value_t logs);

/**
 * Add a list of metrics to this envelope.
 */
sentry_envelope_item_t *sentry__envelope_add_metrics(
    sentry_envelope_t *envelope, sentry_value_t metrics);

/**
 * Add a user feedback to this envelope.
 */
sentry_envelope_item_t *sentry__envelope_add_user_feedback(
    sentry_envelope_t *envelope, sentry_value_t user_feedback);

/**
 * Add a session to this envelope.
 */
sentry_envelope_item_t *sentry__envelope_add_session(
    sentry_envelope_t *envelope, const sentry_session_t *session);

/**
 * Add an attachment to this envelope.
 */
sentry_envelope_item_t *sentry__envelope_add_attachment(
    sentry_envelope_t *envelope, const sentry_attachment_t *attachment);

/**
 * Add normal (non-attachment-ref) attachments to this envelope.
 */
void sentry__envelope_add_attachments(sentry_envelope_t *envelope,
    const sentry_attachment_t *attachments, const sentry_options_t *options);

/**
 * Add an attachment-ref item with standard item headers to this envelope.
 */
sentry_envelope_item_t *sentry__envelope_add_attachment_ref(
    sentry_envelope_t *envelope, const sentry_attachment_ref_t *ref,
    const char *filename, const char *attachment_type,
    size_t attachment_length);

/**
 * Returns true if a client report can be added to the envelope, i.e., the
 * envelope is structured (not raw) and has at least one non-internal item
 * that is not rate-limited.
 */
bool sentry__envelope_can_add_client_report(
    const sentry_envelope_t *envelope, const sentry_rate_limiter_t *rl);

/**
 * Serialize a client report and add it to the envelope.
 */
sentry_envelope_item_t *sentry__envelope_add_client_report(
    sentry_envelope_t *envelope, const sentry_client_report_t *report);

/**
 * Record discards for all non-internal items in the envelope.
 */
void sentry__envelope_discard(const sentry_envelope_t *envelope,
    sentry_discard_reason_t reason, const sentry_rate_limiter_t *rl);

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
 * This sets an explicit header for the given envelope.
 */
void sentry__envelope_set_header(
    sentry_envelope_t *envelope, const char *key, sentry_value_t value);

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

/**
 * Cache the envelope unless it already exists in the cache directory.
 * Minidump attachments are extracted as separate `<uuid>.dmp` files
 * alongside the `<uuid>.envelope` (and excluded from the envelope file).
 */
int sentry__envelope_write_to_cache(
    const sentry_envelope_t *envelope, const sentry_path_t *cache_dir);

bool sentry__envelope_is_raw(const sentry_envelope_t *envelope);

size_t sentry__envelope_get_item_count(const sentry_envelope_t *envelope);
sentry_envelope_item_t *sentry__envelope_get_item(
    const sentry_envelope_t *envelope, size_t idx);
bool sentry__envelope_remove_item(
    sentry_envelope_t *envelope, sentry_envelope_item_t *item);

// these for now are only needed for tests
#ifdef SENTRY_UNITTEST
sentry_value_t sentry__envelope_item_get_header(
    const sentry_envelope_item_t *item, const char *key);
const char *sentry__envelope_item_get_payload(
    const sentry_envelope_item_t *item, size_t *payload_len_out);
#endif

/**
 * Parses `item` as an attachment-ref. The returned field values are valid
 * until `sentry__attachment_ref_cleanup` is called.
 */
bool sentry__envelope_item_get_attachment_ref(
    const sentry_envelope_item_t *item, sentry_attachment_ref_t *ref);

void sentry__attachment_ref_cleanup(sentry_attachment_ref_t *ref);

/**
 * Resolves an attachment-ref item with a remote TUS location.
 */
bool sentry__envelope_item_resolve_attachment_ref(
    sentry_envelope_item_t *item, const char *location);

/**
 * If `envelope` is raw, parse it in place into a structured envelope.
 * No-op if already structured. Returns true on success.
 */
bool sentry__envelope_materialize(sentry_envelope_t *envelope);

/**
 * True if `envelope` contains at least one item with the given `content_type`
 * header value. For raw envelopes this is a byte-substring scan, so use only
 * with sufficiently unique `content_type` strings.
 */
bool sentry__envelope_has_content_type(
    const sentry_envelope_t *envelope, const char *content_type);

#endif
