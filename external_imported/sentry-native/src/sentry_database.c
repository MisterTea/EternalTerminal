#include "sentry_database.h"
#include "sentry_alloc.h"
#include "sentry_attachment.h"
#include "sentry_client_report.h"
#include "sentry_envelope.h"
#include "sentry_json.h"
#include "sentry_options.h"
#include "sentry_session.h"
#include "sentry_slice.h"
#include "sentry_sync.h"
#include "sentry_utils.h"
#include "sentry_uuid.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sentry_run_t *
sentry__run_new(const sentry_path_t *database_path)
{
    sentry_uuid_t uuid = sentry_uuid_new_v4();
    char run_name[46];
    sentry_uuid_as_string(&uuid, run_name);

    // `<db>/<uuid>.run`
    strcpy(&run_name[36], ".run");
    sentry_path_t *run_path = sentry__path_join_str(database_path, run_name);
    if (!run_path) {
        return NULL;
    }

    // `<db>/<uuid>.run.lock`
    strcpy(&run_name[40], ".lock");
    sentry_path_t *lock_path = sentry__path_join_str(database_path, run_name);
    if (!lock_path) {
        sentry__path_free(run_path);
        return NULL;
    }

    // `<db>/<uuid>.run/session.json`
    sentry_path_t *session_path
        = sentry__path_join_str(run_path, "session.json");
    if (!session_path) {
        sentry__path_free(run_path);
        sentry__path_free(lock_path);
        return NULL;
    }

    // `<db>/external`
    sentry_path_t *external_path
        = sentry__path_join_str(database_path, "external");
    if (!external_path) {
        sentry__path_free(run_path);
        sentry__path_free(lock_path);
        sentry__path_free(session_path);
        return NULL;
    }

    // `<db>/cache`
    sentry_path_t *cache_path = sentry__path_join_str(database_path, "cache");
    if (!cache_path) {
        sentry__path_free(run_path);
        sentry__path_free(lock_path);
        sentry__path_free(session_path);
        sentry__path_free(external_path);
        return NULL;
    }

    sentry_run_t *run = SENTRY_MAKE(sentry_run_t);
    if (!run) {
        sentry__path_free(run_path);
        sentry__path_free(session_path);
        sentry__path_free(lock_path);
        sentry__path_free(external_path);
        sentry__path_free(cache_path);
        return NULL;
    }

    run->refcount = 1;
    run->require_user_consent = 0;
    run->user_consent = SENTRY_USER_CONSENT_UNKNOWN;
    run->uuid = uuid;
    run->run_path = run_path;
    run->session_path = session_path;
    run->external_path = external_path;
    run->cache_path = cache_path;
    run->lock = sentry__filelock_new(lock_path);
    if (!run->lock) {
        goto error;
    }
    if (!sentry__filelock_try_lock(run->lock)) {
        SENTRY_WARNF("failed to lock file \"%s\" (%s)", lock_path->path,
            strerror(errno));
        goto error;
    }
    sentry__path_create_dir_all(run->run_path);
    return run;

error:
    sentry__run_free(run);
    return NULL;
}

bool
sentry__run_should_skip_upload(sentry_run_t *run)
{
    return sentry__atomic_fetch(&run->require_user_consent)
        && (sentry__atomic_fetch(&run->user_consent)
            != SENTRY_USER_CONSENT_GIVEN);
}

void
sentry__run_load_user_consent(
    sentry_run_t *run, const sentry_path_t *database_path)
{
    sentry_path_t *consent_path
        = sentry__path_join_str(database_path, "user-consent");
    char *contents = sentry__path_read_to_buffer(consent_path, NULL);
    sentry__path_free(consent_path);
    switch (contents ? contents[0] : 0) {
    case '1':
        sentry__atomic_store(&run->user_consent, SENTRY_USER_CONSENT_GIVEN);
        break;
    case '0':
        sentry__atomic_store(&run->user_consent, SENTRY_USER_CONSENT_REVOKED);
        break;
    default:
        sentry__atomic_store(&run->user_consent, SENTRY_USER_CONSENT_UNKNOWN);
        break;
    }
    sentry_free(contents);
}

void
sentry__run_load_installation_id(sentry_run_t *run,
    const sentry_path_t *database_path, const char *public_key)
{
    sentry_path_t *id_path
        = sentry__path_join_str(database_path, "installation_id");
    if (!id_path) {
        return;
    }

    if (!public_key) {
        public_key = "";
    }
    const size_t key_len = strlen(public_key);

    const size_t uuid_len = 36;
    char uuid_str[37] = { 0 };
    size_t size = 0;
    char *contents = sentry__path_read_to_buffer(id_path, &size);
    if (contents && size >= uuid_len) {
        // expect: "<uuid>(\s+<public_key>)?"
        sentry_slice_t tail = { contents + uuid_len, size - uuid_len };
        sentry_slice_t key = sentry__slice_trim(tail);
        if ((tail.len == 0 || isspace((unsigned char)tail.ptr[0]))
            && sentry__slice_eqs(key, public_key)) {
            memcpy(uuid_str, contents, uuid_len);
            uuid_str[uuid_len] = '\0';
        }
    }
    sentry_free(contents);

    if (uuid_str[0] == '\0') {
        sentry_uuid_t uuid = sentry_uuid_new_v4();
        sentry_uuid_as_string(&uuid, uuid_str);

        const size_t buf_len = uuid_len + 1 + key_len + 1;
        char *buf = sentry_malloc(buf_len);
        if (buf) {
            memcpy(buf, uuid_str, uuid_len);
            buf[uuid_len] = '\n';
            memcpy(buf + uuid_len + 1, public_key, key_len);
            buf[uuid_len + 1 + key_len] = '\n';
            if (sentry__path_write_buffer(id_path, buf, buf_len) != 0) {
                SENTRY_WARN("failed to persist installation ID");
            }
            sentry_free(buf);
        }
    }

    run->installation_id = sentry__string_clone(uuid_str);
    sentry__path_free(id_path);
}

sentry_run_t *
sentry__run_incref(sentry_run_t *run)
{
    if (run) {
        sentry__atomic_fetch_and_add(&run->refcount, 1);
    }
    return run;
}

void
sentry__run_clean(sentry_run_t *run, bool force)
{
    if (!force && sentry__atomic_fetch(&run->retain)) {
        sentry__filelock_unlock(run->lock);
        return;
    }
    sentry__path_remove_all(run->run_path);
    sentry__filelock_unlock(run->lock);
}

void
sentry__run_free(sentry_run_t *run)
{
    if (!run || sentry__atomic_fetch_and_add(&run->refcount, -1) != 1) {
        return;
    }
    sentry__path_free(run->run_path);
    sentry__path_free(run->session_path);
    sentry__path_free(run->external_path);
    sentry__path_free(run->cache_path);
    sentry__filelock_free(run->lock);
    sentry_free(run->installation_id);
    sentry_free(run);
}

static bool
write_envelope(const sentry_path_t *path, const sentry_envelope_t *envelope)
{
    sentry_uuid_t event_id = sentry__envelope_get_event_id(envelope);

    // Generate a random UUID for the filename if the envelope has no event_id
    // this avoids collisions on NIL-UUIDs
    if (sentry_uuid_is_nil(&event_id)) {
        event_id = sentry_uuid_new_v4();
    }

    char *envelope_filename = sentry__uuid_as_filename(&event_id, ".envelope");
    if (!envelope_filename) {
        return false;
    }

    sentry_path_t *output_path = sentry__path_join_str(path, envelope_filename);
    sentry_free(envelope_filename);
    if (!output_path) {
        return false;
    }

    int rv = sentry_envelope_write_to_path(envelope, output_path);
    sentry__path_free(output_path);
    if (rv) {
        SENTRY_WARN("writing envelope to file failed");
        return false;
    }

    return true;
}

// Replace any char outside [A-Za-z0-9._-] with '_', strip leading dots,
// truncate to at most out_size-1 bytes, NUL-terminate. Empty result becomes
// "attachment". Used for the on-disk sibling basename only; the original
// filename is preserved verbatim in the envelope item header.
static void
sanitize_basename(const char *src, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    size_t i = 0;
    if (src) {
        while (*src == '.') {
            src++;
        }
        while (*src && i + 1 < out_size) {
            char c = *src++;
            bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            out[i++] = ok ? c : '_';
        }
    }
    if (i == 0) {
        const char *def = "attachment";
        while (*def && i + 1 < out_size) {
            out[i++] = *def++;
        }
    }
    out[i] = '\0';
}

// Build the cache sibling path, delegating collision avoidance to
// `sentry__path_unique`. Returns a newly-allocated path owned by the caller,
// or NULL on failure.
static sentry_path_t *
build_sibling_path(const sentry_path_t *cache_path, const char *uuid_str,
    const sentry_attachment_t *att)
{
    if (att->type && strcmp(att->type, SENTRY_ATTACHMENT_TYPE_MINIDUMP) == 0) {
        char buf[41];
        snprintf(buf, sizeof(buf), "%s.dmp", uuid_str);
        return sentry__path_unique(cache_path, buf);
    }

    char buf[256];
    memcpy(buf, uuid_str, 36);
    buf[36] = '-';
    sanitize_basename(
        sentry__attachment_get_filename(att), buf + 37, sizeof(buf) - 37);

    return sentry__path_unique(cache_path, buf);
}

// Cache `att` to a sibling file of the cached envelope and append an
// attachment-ref item with `path` set. Returns true on success.
static bool
cache_attachment_ref(sentry_envelope_t *envelope,
    const sentry_attachment_t *att, const sentry_path_t *cache_path,
    const char *uuid_str, const sentry_path_t *run_path)
{
    // File-backed attachments must exist on disk.
    if (!att->buf && !sentry__path_is_file(att->path)) {
        return false;
    }

    if (sentry__path_create_dir_all(cache_path) != 0) {
        return false;
    }

    const char *raw_filename = sentry__attachment_get_filename(att);
    sentry_path_t *dst = build_sibling_path(cache_path, uuid_str, att);
    if (!dst) {
        return false;
    }

    int rv;
    if (att->buf) {
        rv = sentry__path_write_buffer(dst, att->buf, att->buf_len);
    } else {
        sentry_path_t *src_dir = sentry__path_dir(att->path);
        bool is_run_owned
            = run_path && src_dir && sentry__path_eq(src_dir, run_path);
        sentry__path_free(src_dir);
        rv = is_run_owned ? sentry__path_rename(att->path, dst)
                          : sentry__path_copy(att->path, dst);
    }
    if (rv != 0) {
        sentry__path_remove(dst);
        sentry__path_free(dst);
        return false;
    }

    size_t file_size = sentry__path_get_size(dst);
    sentry_attachment_ref_t ref = { 0 };
    ref.path = sentry__path_filename(dst);
    ref.content_type = att->content_type;
    sentry_envelope_item_t *item = sentry__envelope_add_attachment_ref(
        envelope, &ref, raw_filename, att->type, file_size);
    if (!item) {
        sentry__path_remove(dst);
        sentry__path_free(dst);
        return false;
    }
    sentry__path_free(dst);
    return true;
}

bool
sentry__cache_attachment_ref(sentry_envelope_t *envelope,
    const sentry_attachment_t *attachment, const sentry_path_t *cache_path,
    const sentry_path_t *run_path)
{
    if (!envelope || !attachment || !cache_path) {
        return false;
    }

    sentry_uuid_t event_id = sentry__envelope_get_event_id(envelope);
    if (sentry_uuid_is_nil(&event_id)) {
        return false;
    }

    char uuid_str[37];
    sentry_uuid_as_string(&event_id, uuid_str);

    return cache_attachment_ref(
        envelope, attachment, cache_path, uuid_str, run_path);
}

void
sentry__cache_attachment_refs(sentry_envelope_t *envelope,
    const sentry_attachment_t *attachments, const sentry_options_t *options,
    const sentry_path_t *cache_path, const sentry_path_t *run_path)
{
    if (!envelope || !attachments || !cache_path) {
        return;
    }

    sentry_uuid_t event_id = sentry__envelope_get_event_id(envelope);
    if (sentry_uuid_is_nil(&event_id)) {
        return;
    }

    char uuid_str[37];
    sentry_uuid_as_string(&event_id, uuid_str);

    for (const sentry_attachment_t *att = attachments; att; att = att->next) {
        if (!sentry__attachment_is_placeholder(att, options)) {
            continue;
        }
        if (!cache_attachment_ref(
                envelope, att, cache_path, uuid_str, run_path)) {
            SENTRY_WARN("failed to cache attachment-ref");
            sentry__client_report_discard(SENTRY_DISCARD_REASON_SEND_ERROR,
                SENTRY_DATA_CATEGORY_ATTACHMENT, 1);
        }
    }
}

bool
sentry__run_write_envelope(
    const sentry_run_t *run, const sentry_envelope_t *envelope)
{
    if (!write_envelope(run->run_path, envelope)) {
        return false;
    }
    sentry__atomic_store((long *)&run->retain, 1);
    return true;
}

bool
sentry__run_write_external(
    const sentry_run_t *run, const sentry_envelope_t *envelope)
{
    if (sentry__path_create_dir_all(run->external_path) != 0) {
        SENTRY_ERRORF("mkdir failed: \"%s\"", run->external_path->path);
        return false;
    }

    return write_envelope(run->external_path, envelope);
}

bool
sentry__run_write_cache(
    const sentry_run_t *run, const sentry_envelope_t *envelope, int retry_count)
{
    if (sentry__path_create_dir_all(run->cache_path) != 0) {
        SENTRY_ERRORF("mkdir failed: \"%s\"", run->cache_path->path);
        return false;
    }

    if (retry_count < 0) {
        return sentry__envelope_write_to_cache(envelope, run->cache_path) == 0;
    }

    sentry_uuid_t event_id = sentry__envelope_get_event_id(envelope);
    if (sentry_uuid_is_nil(&event_id)) {
        event_id = sentry_uuid_new_v4();
    }

    char uuid[37];
    sentry_uuid_as_string(&event_id, uuid);

    sentry_path_t *path = sentry__run_make_cache_path(
        run, sentry__usec_time() / 1000, retry_count, uuid);
    if (!path) {
        return false;
    }

    int rv = sentry_envelope_write_to_path(envelope, path);
    sentry__path_free(path);
    if (rv) {
        SENTRY_WARN("writing envelope to file failed");
    }
    return rv == 0;
}

bool
sentry__parse_cache_filename(const char *filename, uint64_t *ts_out,
    int *count_out, const char **uuid_out)
{
    if (!filename) {
        return false;
    }
    size_t len = strlen(filename);

    // Bare cache filename: <uuid>.envelope (exactly 45 chars).
    if (len == 36 + 9 && strcmp(filename + 36, ".envelope") == 0) {
        *ts_out = 0;
        *count_out = -1;
        *uuid_out = filename;
        return true;
    }

    // Retry filename: <ts>-<count>-<uuid>.envelope (49+ chars).
    if (len <= 45) {
        return false;
    }

    char *end;
    uint64_t ts = strtoull(filename, &end, 10);
    if (*end != '-') {
        return false;
    }

    const char *count_str = end + 1;
    long count = strtol(count_str, &end, 10);
    if (*end != '-' || count < 0) {
        return false;
    }

    const char *uuid = end + 1;
    size_t tail_len = strlen(uuid);
    // 36 chars UUID (with dashes) + ".envelope"
    if (tail_len != 36 + 9 || strcmp(uuid + 36, ".envelope") != 0) {
        return false;
    }

    *ts_out = ts;
    *count_out = (int)count;
    *uuid_out = uuid;
    return true;
}

sentry_path_t *
sentry__run_make_cache_path(
    const sentry_run_t *run, uint64_t ts, int count, const char *uuid)
{
    char filename[128];
    if (count >= 0) {
        snprintf(filename, sizeof(filename), "%" PRIu64 "-%02d-%.36s.envelope",
            ts, count, uuid);
    } else {
        snprintf(filename, sizeof(filename), "%.36s.envelope", uuid);
    }
    return sentry__path_join_str(run->cache_path, filename);
}

bool
sentry__run_move_cache(
    const sentry_run_t *run, const sentry_path_t *src, int retry_count)
{
    if (sentry__path_create_dir_all(run->cache_path) != 0) {
        SENTRY_ERRORF("mkdir failed: \"%s\"", run->cache_path->path);
        return false;
    }

    const char *src_name = sentry__path_filename(src);
    uint64_t parsed_ts;
    int parsed_count;
    const char *parsed_uuid;
    const char *cache_name = sentry__parse_cache_filename(src_name, &parsed_ts,
                                 &parsed_count, &parsed_uuid)
        ? parsed_uuid
        : src_name;

    sentry_path_t *dst_path = sentry__run_make_cache_path(
        run, sentry__usec_time() / 1000, retry_count, cache_name);
    if (!dst_path) {
        return false;
    }

    int rv = sentry__path_rename(src, dst_path);
    sentry__path_free(dst_path);
    if (rv != 0) {
        SENTRY_WARNF("failed to cache envelope \"%s\"", src_name);
        return false;
    }

    return true;
}

bool
sentry__run_write_session(
    const sentry_run_t *run, const sentry_session_t *session)
{
    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_sb(NULL);
    if (!jw) {
        return false;
    }
    sentry__session_to_json(session, jw);
    size_t buf_len;
    char *buf = sentry__jsonwriter_into_string(jw, &buf_len);
    if (!buf) {
        return false;
    }

    int rv = sentry__path_write_buffer(run->session_path, buf, buf_len);
    sentry_free(buf);

    if (rv) {
        SENTRY_WARN("writing session to file failed");
    }
    return !rv;
}

bool
sentry__run_clear_session(const sentry_run_t *run)
{
    int rv = sentry__path_remove(run->session_path);
    return !rv;
}

void
sentry__process_old_runs(const sentry_options_t *options, uint64_t last_crash)
{
    sentry_pathiter_t *db_iter
        = sentry__path_iter_directory(options->database_path);
    if (!db_iter) {
        return;
    }
    const sentry_path_t *run_dir;
    sentry_envelope_t *session_envelope = NULL;
    size_t session_num = 0;

    while ((run_dir = sentry__pathiter_next(db_iter)) != NULL) {
        // skip over other files such as the saved consent or the last_crash
        // timestamp
        if (!sentry__path_is_dir(run_dir) || sentry__path_is_symlink(run_dir)) {
            continue;
        }

        // prune 1h old external crash report files
        if (sentry__path_filename_matches(run_dir, "external")) {
            time_t now = time(NULL);
            sentry_pathiter_t *it = sentry__path_iter_directory(run_dir);
            const sentry_path_t *file;
            while (it && (file = sentry__pathiter_next(it)) != NULL) {
                time_t age = now - sentry__path_get_mtime(file);
                if (age / 3600 > 0) {
                    sentry__path_remove(file);
                }
            }
            sentry__pathiter_free(it);
            continue;
        }

        if (!sentry__path_ends_with(run_dir, ".run")) {
            continue;
        }

        // our own run directory is protected by options->run->lock
        if (sentry__path_eq(run_dir, options->run->run_path)) {
            continue;
        }

        sentry_path_t *lockfile = sentry__path_append_str(run_dir, ".lock");
        if (!lockfile) {
            continue;
        }
        sentry_filelock_t *lock = sentry__filelock_new(lockfile);
        if (!lock) {
            continue;
        }
        bool did_lock = sentry__filelock_try_lock(lock);
        // the file is locked by another process
        if (!did_lock) {
            sentry__filelock_free(lock);
            continue;
        }

        sentry_pathiter_t *run_iter = sentry__path_iter_directory(run_dir);
        const sentry_path_t *file;
        while (run_iter && (file = sentry__pathiter_next(run_iter)) != NULL) {
            if (sentry__path_filename_matches(file, "session.json")) {
                if (!session_envelope) {
                    session_envelope = sentry__envelope_new();
                }
                sentry_session_t *session = sentry__session_from_path(file);
                if (session) {
                    // this is just a heuristic: whenever the session was not
                    // closed properly, and we do have a crash that happened
                    // *after* the session was started, we will assume that the
                    // crash corresponds to the session and flag it as crashed.
                    // this should only happen when using crashpad, and there
                    // should normally be only a single unclosed session at a
                    // time.
                    if (session->status == SENTRY_SESSION_STATUS_OK) {
                        bool was_crash
                            = last_crash && last_crash > session->started_us;
                        if (was_crash) {
                            session->duration_us
                                = last_crash - session->started_us;
                            session->errors += 1;
                            // we only set at most one unclosed session as
                            // crashed
                            last_crash = 0;
                        }
                        session->status = was_crash
                            ? SENTRY_SESSION_STATUS_CRASHED
                            : SENTRY_SESSION_STATUS_ABNORMAL;
                    }
                    sentry__envelope_add_session(session_envelope, session);

                    sentry__session_free(session);
                    if ((++session_num) >= SENTRY_MAX_ENVELOPE_SESSIONS) {
                        sentry__capture_envelope(
                            options->transport, session_envelope, options);
                        session_envelope = NULL;
                        session_num = 0;
                    }
                }
            } else if (sentry__path_ends_with(file, ".envelope")) {
                sentry_envelope_t *envelope = sentry__envelope_from_path(file);
                if (envelope) {
                    sentry__capture_envelope(
                        options->transport, envelope, options);
                }
            }

            sentry__path_remove(file);
        }
        sentry__pathiter_free(run_iter);

        sentry__path_remove_all(run_dir);
        sentry__filelock_free(lock);
    }
    sentry__pathiter_free(db_iter);

    if (session_envelope) {
        sentry__capture_envelope(options->transport, session_envelope, options);
    }
}

// Cache Pruning below is based on prune_crash_reports.cc from Crashpad

/**
 * A cache entry with its metadata for sorting and pruning decisions.
 */
typedef struct {
    sentry_path_t *path;
    time_t mtime;
    size_t size;
} cache_entry_t;

typedef struct cache_sibling_s {
    sentry_path_t *path;
    struct cache_sibling_s *next;
} cache_sibling_t;

/**
 * Comparison function to sort cache entries by mtime, newest first.
 */
static int
compare_cache_entries_newest_first(const void *a, const void *b)
{
    const cache_entry_t *entry_a = (const cache_entry_t *)a;
    const cache_entry_t *entry_b = (const cache_entry_t *)b;
    // Newest first: if b is newer, return positive (b comes before a)
    if (entry_b->mtime > entry_a->mtime) {
        return 1;
    }
    if (entry_b->mtime < entry_a->mtime) {
        return -1;
    }
    return 0;
}

static bool
is_cache_sibling(const char *name, const char *uuid)
{
    if (!name || strncmp(name, uuid, 36) != 0) {
        return false;
    }

    char c = name[36];
    if (c != '.' && c != '-') {
        return false;
    }

    return c != '.' || strcmp(name + 36, ".envelope") != 0;
}

/**
 * Iterate over cache-sibling files for a given .envelope path. A sibling is
 * any file in the same directory whose name starts with `<uuid>.` or
 * `<uuid>-`, where `<uuid>` is the event UUID extracted from the envelope
 * filename. This works for both `<uuid>.envelope` and retry-renamed
 * `<ts>-<count>-<uuid>.envelope` files.
 */
static void
foreach_cache_sibling(const sentry_path_t *envelope_path,
    void (*callback)(sentry_path_t *path, void *data), void *data)
{
    const char *envelope_name = sentry__path_filename(envelope_path);
    uint64_t parsed_ts = 0;
    int parsed_count = 0;
    const char *parsed_uuid = NULL;
    if (!sentry__parse_cache_filename(
            envelope_name, &parsed_ts, &parsed_count, &parsed_uuid)) {
        return;
    }
    sentry_path_t *dir = sentry__path_dir(envelope_path);
    if (!dir) {
        return;
    }

    // Snapshot before invoking callbacks that may remove directory entries.
    cache_sibling_t *siblings = NULL;
    cache_sibling_t *tail = NULL;
    sentry_pathiter_t *it = sentry__path_iter_directory(dir);
    const sentry_path_t *entry;
    while (it && (entry = sentry__pathiter_next(it)) != NULL) {
        const char *name = sentry__path_filename(entry);
        if (!is_cache_sibling(name, parsed_uuid)) {
            continue;
        }
        cache_sibling_t *sibling = SENTRY_MAKE(cache_sibling_t);
        if (!sibling) {
            break;
        }
        sibling->path = sentry__path_clone(entry);
        if (!sibling->path) {
            sentry_free(sibling);
            break;
        }
        if (tail) {
            tail->next = sibling;
        } else {
            siblings = sibling;
        }
        tail = sibling;
    }
    sentry__pathiter_free(it);
    sentry__path_free(dir);

    while (siblings) {
        cache_sibling_t *sibling = siblings;
        siblings = sibling->next;
        callback(sibling->path, data);
        sentry__path_free(sibling->path);
        sentry_free(sibling);
    }
}

static void
accumulate_size(sentry_path_t *path, void *data)
{
    size_t *size = data;
    *size += sentry__path_get_size(path);
}

static void
remove_file(sentry_path_t *path, void *data)
{
    (void)data;
    sentry__path_remove(path);
}

void
sentry__cache_remove_envelope(const sentry_path_t *envelope_path)
{
    if (!envelope_path) {
        return;
    }
    foreach_cache_sibling(envelope_path, remove_file, NULL);
    sentry__path_remove(envelope_path);
}

void
sentry__cache_remove_siblings(
    const sentry_run_t *run, const sentry_uuid_t *event_id)
{
    if (!run || sentry_uuid_is_nil(event_id)) {
        return;
    }

    char uuid[37];
    sentry_uuid_as_string(event_id, uuid);
    sentry_path_t *path = sentry__run_make_cache_path(run, 0, -1, uuid);
    if (!path) {
        return;
    }
    foreach_cache_sibling(path, remove_file, NULL);
    sentry__path_free(path);
}

void
sentry__cleanup_cache(const sentry_options_t *options)
{
    if (!options->database_path) {
        return;
    }

    sentry_path_t *cache_dir
        = sentry__path_join_str(options->database_path, "cache");
    if (!cache_dir || !sentry__path_is_dir(cache_dir)) {
        sentry__path_free(cache_dir);
        return;
    }

    // First pass: collect .envelope entries with their metadata. Sibling
    // files are skipped here; their sizes are added to the matching
    // .envelope entry and they are deleted together.
    size_t entries_capacity = 16;
    size_t entries_count = 0;
    cache_entry_t *entries
        = sentry_malloc(sizeof(cache_entry_t) * entries_capacity);
    if (!entries) {
        sentry__path_free(cache_dir);
        return;
    }

    sentry_pathiter_t *iter = sentry__path_iter_directory(cache_dir);
    const sentry_path_t *entry;
    while (iter && (entry = sentry__pathiter_next(iter)) != NULL) {
        if (sentry__path_is_dir(entry)
            || !sentry__path_ends_with(entry, ".envelope")) {
            continue;
        }

        // Grow array if needed
        if (entries_count >= entries_capacity) {
            entries_capacity *= 2;
            cache_entry_t *new_entries
                = sentry_malloc(sizeof(cache_entry_t) * entries_capacity);
            if (!new_entries) {
                break;
            }
            memcpy(new_entries, entries, sizeof(cache_entry_t) * entries_count);
            sentry_free(entries);
            entries = new_entries;
        }

        entries[entries_count].path = sentry__path_clone(entry);
        if (!entries[entries_count].path) {
            break;
        }
        entries[entries_count].mtime = sentry__path_get_mtime(entry);
        entries[entries_count].size = sentry__path_get_size(entry);

        // include sizes of matching <uuid>.dmp / <uuid>-* siblings
        foreach_cache_sibling(
            entry, accumulate_size, &entries[entries_count].size);

        entries_count++;
    }
    sentry__pathiter_free(iter);

    // Sort by mtime, newest first (like crashpad)
    // This ensures we keep the newest entries when pruning by size
    qsort(entries, entries_count, sizeof(cache_entry_t),
        compare_cache_entries_newest_first);

    // Calculate the age threshold
    time_t now = time(NULL);
    time_t oldest_allowed = now - options->cache_max_age;

    // Prune entries: iterate newest-to-oldest, accumulating size
    // Remove if: too old OR accumulated size exceeds limit
    size_t accumulated_size = 0;
    for (size_t i = 0; i < entries_count; i++) {
        bool should_prune = false;

        // Age-based pruning
        if (options->cache_max_age > 0 && entries[i].mtime < oldest_allowed) {
            should_prune = true;
        } else {
            // Size-based pruning (accumulate size as we go, like crashpad)
            accumulated_size += entries[i].size;
            if (options->cache_max_size > 0
                && accumulated_size > options->cache_max_size) {
                should_prune = true;
            }
            // Item count pruning
            if (options->cache_max_items > 0 && i >= options->cache_max_items) {
                should_prune = true;
            }
        }

        if (should_prune) {
            foreach_cache_sibling(entries[i].path, remove_file, NULL);
            sentry__path_remove_all(entries[i].path);
        }
        sentry__path_free(entries[i].path);
    }

    sentry_free(entries);
    sentry__path_free(cache_dir);
}

static const char *g_last_crash_filename = "last_crash";

bool
sentry__write_crash_marker(const sentry_options_t *options)
{
    char *iso_time = sentry__usec_time_to_iso8601(sentry__usec_time());
    if (!iso_time) {
        return false;
    }

    sentry_path_t *marker_path
        = sentry__path_join_str(options->database_path, g_last_crash_filename);
    if (!marker_path) {
        sentry_free(iso_time);
        return false;
    }

    size_t iso_time_len = strlen(iso_time);
    int rv = sentry__path_write_buffer(marker_path, iso_time, iso_time_len);
    sentry_free(iso_time);
    sentry__path_free(marker_path);

    if (rv) {
        SENTRY_WARN("writing crash timestamp to file failed");
    }
    return !rv;
}

bool
sentry__has_crash_marker(const sentry_options_t *options)
{
    sentry_path_t *marker_path
        = sentry__path_join_str(options->database_path, g_last_crash_filename);
    if (!marker_path) {
        return false;
    }

    bool result = sentry__path_is_file(marker_path);
    sentry__path_free(marker_path);
    return result;
}

bool
sentry__clear_crash_marker(const sentry_options_t *options)
{
    sentry_path_t *marker_path
        = sentry__path_join_str(options->database_path, g_last_crash_filename);
    if (!marker_path) {
        return false;
    }

    int rv = sentry__path_remove(marker_path);
    sentry__path_free(marker_path);
    if (rv) {
        SENTRY_WARN("removing the crash timestamp file has failed");
    }
    return !rv;
}
