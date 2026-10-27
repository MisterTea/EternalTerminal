#ifndef SENTRY_DATABASE_H_INCLUDED
#define SENTRY_DATABASE_H_INCLUDED

#include "sentry_boot.h"

#include "sentry_attachment.h"
#include "sentry_path.h"
#include "sentry_session.h"

typedef struct sentry_run_s {
    sentry_uuid_t uuid;
    sentry_path_t *run_path;
    sentry_path_t *session_path;
    sentry_path_t *external_path;
    sentry_path_t *cache_path;
    sentry_filelock_t *lock;
    long refcount;
    long retain; // (atomic) bool
    long require_user_consent; // (atomic) bool
    long user_consent; // (atomic) sentry_user_consent_t
    char *installation_id;
} sentry_run_t;

/**
 * This function will check the user consent, and return `true` if uploads
 * should *not* be sent to the sentry server, and be discarded instead.
 *
 * This is a lock-free variant of `sentry__should_skip_upload`, safe to call
 * from worker threads while the options are locked during SDK shutdown.
 */
bool sentry__run_should_skip_upload(sentry_run_t *run);

/**
 * Loads the persisted user consent (`<database>/user-consent`) into the run.
 */
void sentry__run_load_user_consent(
    sentry_run_t *run, const sentry_path_t *database_path);

/**
 * Loads or creates the persisted installation ID. The file
 * `<database>/installation_id` stores a UUIDv4 on line 1 and the given
 * `public_key` on line 2. If the stored key matches, the UUID is reused;
 * otherwise a new one is generated and the file is rewritten. A NULL
 * `public_key` is treated as an empty string.
 */
void sentry__run_load_installation_id(sentry_run_t *run,
    const sentry_path_t *database_path, const char *public_key);

/**
 * This creates a new application run including its associated directory and
 * lockfile:
 * * `<database>/<uuid>.run/`
 * * `<database>/<uuid>.run.lock`
 */
sentry_run_t *sentry__run_new(const sentry_path_t *database_path);

/**
 * Increment the refcount and return the run pointer.
 */
sentry_run_t *sentry__run_incref(sentry_run_t *run);

/**
 * This will clean up all the files belonging to this run.
 */
void sentry__run_clean(sentry_run_t *run, bool force);

/**
 * Free the previously allocated run.
 * Make sure to call `sentry__run_clean` first, to not leave any files or
 * directories laying around.
 */
void sentry__run_free(sentry_run_t *run);

/**
 * This will serialize and write the given envelope to disk into a file named
 * like so:
 * `<database>/<uuid>.run/<event-uuid>.envelope`
 */
bool sentry__run_write_envelope(
    const sentry_run_t *run, const sentry_envelope_t *envelope);

/**
 * Cache an attachment to a sibling of the cached envelope and append an
 * `attachment-ref` item whose payload carries the on-disk basename in the
 * `path` field. Regular attachments are stored as
 * `<event_id>-<sanitized-basename>`; minidumps keep the legacy
 * `<event_id>.dmp` shape.
 *
 * `run_path` enables a rename (instead of a copy) when the source is inside
 * that directory; pass NULL to always copy.
 */
bool sentry__cache_attachment_ref(sentry_envelope_t *envelope,
    const sentry_attachment_t *attachment, const sentry_path_t *cache_path,
    const sentry_path_t *run_path);

/**
 * Cache every attachment that should be represented as an attachment-ref.
 */
void sentry__cache_attachment_refs(sentry_envelope_t *envelope,
    const sentry_attachment_t *attachments, const sentry_options_t *options,
    const sentry_path_t *cache_path, const sentry_path_t *run_path);

/**
 * This will serialize and write the given envelope to disk into a file named
 * like so:
 * `<database>/external/<event-uuid>.envelope`
 */
bool sentry__run_write_external(
    const sentry_run_t *run, const sentry_envelope_t *envelope);

/**
 * This will serialize and write the given session to disk into a file named:
 * `<database>/<uuid>.run/session.json`
 */
bool sentry__run_write_session(
    const sentry_run_t *run, const sentry_session_t *session);

/**
 * This will remove any previously created session file.
 * See `sentry__run_write_session`.
 */
bool sentry__run_clear_session(const sentry_run_t *run);

/**
 * This will serialize and write the given envelope to disk into the cache
 * directory. When retry_count >= 0 the filename uses retry format
 * `<ts>-<count>-<uuid>.envelope`, otherwise `<uuid>.envelope`.
 */
bool sentry__run_write_cache(const sentry_run_t *run,
    const sentry_envelope_t *envelope, int retry_count);

/**
 * Moves a file into the cache directory. When retry_count >= 0 the
 * destination uses retry format `<ts>-<count>-<uuid>.envelope`,
 * otherwise the original filename is preserved.
 */
bool sentry__run_move_cache(
    const sentry_run_t *run, const sentry_path_t *src, int retry_count);

/**
 * Builds a cache path. When count >= 0 the result is
 * `<db>/cache/<ts>-<count>-<uuid>.envelope`, otherwise
 * `<db>/cache/<uuid>.envelope`.
 */
sentry_path_t *sentry__run_make_cache_path(
    const sentry_run_t *run, uint64_t ts, int count, const char *uuid);

/**
 * This function is essential to send crash reports from previous runs of the
 * program.
 * More specifically, this function will iterate over all the  directories
 * inside the `database_path`. Directories matching `<database>/<uuid>.run/`
 * will be locked, and any files named  `<event-uuid>.envelope` or
 * `session.json` will be queued for sending to the  backend. The files and
 * directories matching these criteria will be deleted afterwards.
 * The following heuristic is applied to all unclosed sessions: If the session
 * was started before the timestamp given by `last_crash`, the session is closed
 * as "crashed" with an appropriate duration.
 */
void sentry__process_old_runs(
    const sentry_options_t *options, uint64_t last_crash);

/**
 * Parses a cache filename in either form:
 *   - `<uuid>.envelope` sets `*ts_out = 0`, `*count_out = -1`.
 *   - `<ts>-<count>-<uuid>.envelope` is retry form, count >= 0.
 * `*uuid_out` points into `filename` at the start of the 36-char UUID.
 * Callers that want only retry-format entries should additionally check
 * `count >= 0`.
 */
bool sentry__parse_cache_filename(const char *filename, uint64_t *ts_out,
    int *count_out, const char **uuid_out);

/**
 * Removes an envelope and any cache siblings sharing the same UUID prefix.
 */
void sentry__cache_remove_envelope(const sentry_path_t *envelope_path);

/**
 * Removes cache siblings sharing the given event UUID prefix.
 */
void sentry__cache_remove_siblings(
    const sentry_run_t *run, const sentry_uuid_t *event_id);

/**
 * Cleans up the cache based on options.cache_max_items,
 * options.cache_max_size and options.cache_max_age.
 */
void sentry__cleanup_cache(const sentry_options_t *options);

/**
 * This will write the current ISO8601 formatted timestamp into the
 * `<database>/last_crash` file.
 */
bool sentry__write_crash_marker(const sentry_options_t *options);

/**
 * This will check whether the `<database>/last_crash` file exists.
 */
bool sentry__has_crash_marker(const sentry_options_t *options);

/**
 * This will remove the `<database>/last_crash` file.
 */
bool sentry__clear_crash_marker(const sentry_options_t *options);

#endif
