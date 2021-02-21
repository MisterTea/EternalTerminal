#include "sentry_database.h"
#include "sentry_alloc.h"
#include "sentry_envelope.h"
#include "sentry_json.h"
#include "sentry_options.h"
#include "sentry_session.h"
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

    sentry_run_t *run = SENTRY_MAKE(sentry_run_t);
    if (!run) {
        sentry__path_free(run_path);
        sentry__path_free(session_path);
        sentry__path_free(lock_path);
        return NULL;
    }

    run->uuid = uuid;
    run->run_path = run_path;
    run->session_path = session_path;
    run->lock = sentry__filelock_new(lock_path);
    if (!run->lock || !sentry__filelock_try_lock(run->lock)) {
        sentry__run_free(run);
        return NULL;
    }

    sentry__path_create_dir_all(run->run_path);
    return run;
}

void
sentry__run_clean(sentry_run_t *run)
{
    sentry__path_remove_all(run->run_path);
    sentry__filelock_unlock(run->lock);
}

void
sentry__run_free(sentry_run_t *run)
{
    if (!run) {
        return;
    }
    sentry__path_free(run->run_path);
    sentry__path_free(run->session_path);
    sentry__filelock_free(run->lock);
    sentry_free(run);
}

bool
sentry__run_write_envelope(
    const sentry_run_t *run, const sentry_envelope_t *envelope)
{
    // 37 for the uuid, 9 for the `.envelope` suffix
    char envelope_filename[37 + 9];
    sentry_uuid_t event_id = sentry__envelope_get_event_id(envelope);
    sentry_uuid_as_string(&event_id, envelope_filename);
    strcpy(&envelope_filename[36], ".envelope");

    sentry_path_t *output_path
        = sentry__path_join_str(run->run_path, envelope_filename);
    if (!output_path) {
        return false;
    }

    int rv = sentry_envelope_write_to_path(envelope, output_path);
    sentry__path_free(output_path);

    if (rv) {
        SENTRY_DEBUG("writing envelope to file failed");
    }

    // the `write_to_path` returns > 0 on failure, but we would like a real bool
    return !rv;
}

bool
sentry__run_write_session(
    const sentry_run_t *run, const sentry_session_t *session)
{
    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_in_memory();
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
        SENTRY_DEBUG("writing session to file failed");
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
        if (!sentry__path_is_dir(run_dir)
            || !sentry__path_ends_with(run_dir, ".run")) {
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
        while ((file = sentry__pathiter_next(run_iter)) != NULL) {
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
                            = last_crash && last_crash > session->started_ms;
                        if (was_crash) {
                            session->duration_ms
                                = last_crash - session->started_ms;
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
                    if ((++session_num) >= SENTRY_MAX_ENVELOPE_ITEMS) {
                        sentry__capture_envelope(
                            options->transport, session_envelope);
                        session_envelope = NULL;
                        session_num = 0;
                    }
                }
            } else if (sentry__path_ends_with(file, ".envelope")) {
                sentry_envelope_t *envelope = sentry__envelope_from_path(file);
                sentry__capture_envelope(options->transport, envelope);
            }

            sentry__path_remove(file);
        }
        sentry__pathiter_free(run_iter);

        sentry__path_remove_all(run_dir);
        sentry__filelock_free(lock);
    }
    sentry__pathiter_free(db_iter);

    sentry__capture_envelope(options->transport, session_envelope);
}

bool
sentry__write_crash_marker(const sentry_options_t *options)
{
    char *iso_time = sentry__msec_time_to_iso8601(sentry__msec_time());
    if (!iso_time) {
        return false;
    }

    sentry_path_t *marker_path
        = sentry__path_join_str(options->database_path, "last_crash");
    if (!marker_path) {
        sentry_free(iso_time);
        return false;
    }

    size_t iso_time_len = strlen(iso_time);
    int rv = sentry__path_write_buffer(marker_path, iso_time, iso_time_len);
    sentry_free(iso_time);
    sentry__path_free(marker_path);

    if (rv) {
        SENTRY_DEBUG("writing crash timestamp to file failed");
    }
    return !rv;
}
