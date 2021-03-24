#include "sentry_session.h"
#include "sentry_alloc.h"
#include "sentry_envelope.h"
#include "sentry_json.h"
#include "sentry_options.h"
#include "sentry_scope.h"
#include "sentry_string.h"
#include "sentry_utils.h"
#include "sentry_value.h"

#include <assert.h>
#include <string.h>

static const char *
status_as_string(sentry_session_status_t status)
{
    switch (status) {
    case SENTRY_SESSION_STATUS_OK:
        return "ok";
    case SENTRY_SESSION_STATUS_CRASHED:
        return "crashed";
    case SENTRY_SESSION_STATUS_ABNORMAL:
        return "abnormal";
    case SENTRY_SESSION_STATUS_EXITED:
        return "exited";
    default:
        assert(!"invalid session status");
        return "invalid";
    }
}

static sentry_session_status_t
status_from_string(const char *status)
{
    if (sentry__string_eq(status, "ok")) {
        return SENTRY_SESSION_STATUS_OK;
    } else if (sentry__string_eq(status, "exited")) {
        return SENTRY_SESSION_STATUS_EXITED;
    } else if (sentry__string_eq(status, "crashed")) {
        return SENTRY_SESSION_STATUS_CRASHED;
    } else if (sentry__string_eq(status, "abnormal")) {
        return SENTRY_SESSION_STATUS_ABNORMAL;
    } else {
        return SENTRY_SESSION_STATUS_OK;
    }
}

sentry_session_t *
sentry__session_new(void)
{
    char *release = NULL;
    char *environment = NULL;
    SENTRY_WITH_OPTIONS (options) {
        release = sentry__string_clone(sentry_options_get_release(options));
        environment
            = sentry__string_clone(sentry_options_get_environment(options));
    }

    if (!release) {
        sentry_free(environment);
        return NULL;
    }

    sentry_session_t *rv = SENTRY_MAKE(sentry_session_t);
    if (!rv) {
        sentry_free(release);
        sentry_free(environment);
        return NULL;
    }

    rv->release = release;
    rv->environment = environment;
    rv->session_id = sentry_uuid_new_v4();
    rv->distinct_id = sentry_value_new_null();
    rv->status = SENTRY_SESSION_STATUS_OK;
    rv->init = true;
    rv->errors = 0;
    rv->started_ms = sentry__msec_time();
    rv->duration_ms = (uint64_t)-1;

    return rv;
}

void
sentry__session_free(sentry_session_t *session)
{
    if (!session) {
        return;
    }
    sentry_value_decref(session->distinct_id);
    sentry_free(session->release);
    sentry_free(session->environment);
    sentry_free(session);
}

void
sentry__session_to_json(
    const sentry_session_t *session, sentry_jsonwriter_t *jw)
{
    sentry__jsonwriter_write_object_start(jw);
    if (session->init) {
        sentry__jsonwriter_write_key(jw, "init");
        sentry__jsonwriter_write_bool(jw, true);
    }
    sentry__jsonwriter_write_key(jw, "sid");
    sentry__jsonwriter_write_uuid(jw, &session->session_id);
    sentry__jsonwriter_write_key(jw, "status");
    sentry__jsonwriter_write_str(jw, status_as_string(session->status));
    if (!sentry_value_is_null(session->distinct_id)) {
        char *did = sentry__value_stringify(session->distinct_id);
        if (did) {
            sentry__jsonwriter_write_key(jw, "did");
            sentry__jsonwriter_write_str(jw, did);
            sentry_free(did);
        }
    }
    sentry__jsonwriter_write_key(jw, "errors");
    sentry__jsonwriter_write_int32(jw, (int32_t)session->errors);

    sentry__jsonwriter_write_key(jw, "started");
    sentry__jsonwriter_write_msec_timestamp(jw, session->started_ms);

    // if there is a duration stored on the struct (that happens after
    // reading back from disk) we use that, otherwise we calculate the
    // difference to the start time.
    sentry__jsonwriter_write_key(jw, "duration");
    double duration;
    if (session->duration_ms != (uint64_t)-1) {
        duration = (double)session->duration_ms / 1000.0;
    } else {
        duration = (double)(sentry__msec_time() - session->started_ms) / 1000.0;
    }
    sentry__jsonwriter_write_double(jw, duration);

    sentry__jsonwriter_write_key(jw, "attrs");
    sentry__jsonwriter_write_object_start(jw);
    sentry__jsonwriter_write_key(jw, "release");
    sentry__jsonwriter_write_str(jw, session->release);
    sentry__jsonwriter_write_key(jw, "environment");
    sentry__jsonwriter_write_str(jw, session->environment);
    sentry__jsonwriter_write_object_end(jw);

    sentry__jsonwriter_write_object_end(jw);
}

sentry_session_t *
sentry__session_from_json(const char *buf, size_t buflen)
{
    sentry_value_t value = sentry__value_from_json(buf, buflen);
    if (sentry_value_is_null(value)) {
        return NULL;
    }

    sentry_value_t attrs = sentry_value_get_by_key(value, "attrs");
    if (sentry_value_is_null(attrs)) {
        return NULL;
    }
    char *release = sentry__string_clone(
        sentry_value_as_string(sentry_value_get_by_key(attrs, "release")));
    if (!release) {
        return NULL;
    }

    sentry_session_t *rv = SENTRY_MAKE(sentry_session_t);
    if (!rv) {
        sentry_free(release);
        return NULL;
    }
    rv->session_id
        = sentry__value_as_uuid(sentry_value_get_by_key(value, "sid"));

    rv->distinct_id = sentry_value_get_by_key_owned(value, "did");

    rv->release = release;
    rv->environment = sentry__string_clone(
        sentry_value_as_string(sentry_value_get_by_key(attrs, "environment")));

    const char *status
        = sentry_value_as_string(sentry_value_get_by_key(value, "status"));
    rv->status = status_from_string(status);

    rv->init = sentry_value_is_true(sentry_value_get_by_key(value, "init"));

    rv->errors = (int64_t)sentry_value_as_int32(
        sentry_value_get_by_key(value, "errors"));
    rv->started_ms = sentry__iso8601_to_msec(
        sentry_value_as_string(sentry_value_get_by_key(value, "started")));
    rv->duration_ms = (uint64_t)(
        sentry_value_as_double(sentry_value_get_by_key(value, "duration"))
        * 1000);

    sentry_value_decref(value);

    return rv;
}

sentry_session_t *
sentry__session_from_path(const sentry_path_t *path)
{
    size_t buf_len;
    char *buf = sentry__path_read_to_buffer(path, &buf_len);
    if (!buf) {
        return NULL;
    }

    sentry_session_t *rv = sentry__session_from_json(buf, buf_len);
    sentry_free(buf);
    return rv;
}

void
sentry_start_session(void)
{
    sentry_end_session();
    SENTRY_WITH_SCOPE_MUT (scope) {
        scope->session = sentry__session_new();
        sentry__scope_session_sync(scope);
    }
}

void
sentry__record_errors_on_current_session(uint32_t error_count)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        if (scope->session) {
            scope->session->errors += error_count;
        }
    }
}

static sentry_session_t *
sentry__end_session_internal(void)
{
    sentry_session_t *session = NULL;
    SENTRY_WITH_SCOPE_MUT (scope) {
        session = scope->session;
        scope->session = NULL;
    }

    if (session && session->status == SENTRY_SESSION_STATUS_OK) {
        session->status = SENTRY_SESSION_STATUS_EXITED;
    }
    return session;
}

sentry_session_t *
sentry__end_current_session_with_status(sentry_session_status_t status)
{
    sentry_session_t *session = sentry__end_session_internal();
    if (session) {
        session->status = status;
    }
    return session;
}

void
sentry_end_session(void)
{
    sentry_session_t *session = sentry__end_session_internal();
    if (!session) {
        return;
    }

    sentry_envelope_t *envelope = sentry__envelope_new();
    sentry__envelope_add_session(envelope, session);
    sentry__session_free(session);

    SENTRY_WITH_OPTIONS (options) {
        sentry__capture_envelope(options->transport, envelope);
    }
}

void
sentry__add_current_session_to_envelope(sentry_envelope_t *envelope)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        if (scope->session) {
            sentry__envelope_add_session(envelope, scope->session);
            // we're assuming that if a session is added to an envelope it
            // will be sent onwards.  This means we now need to set the init
            // flag to false because we're no longer the initial session update.
            scope->session->init = false;
        }
    }
}
