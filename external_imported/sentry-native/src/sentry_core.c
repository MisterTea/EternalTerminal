#include "sentry_boot.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "sentry_alloc.h"
#include "sentry_backend.h"
#include "sentry_core.h"
#include "sentry_database.h"
#include "sentry_envelope.h"
#include "sentry_options.h"
#include "sentry_path.h"
#include "sentry_random.h"
#include "sentry_scope.h"
#include "sentry_session.h"
#include "sentry_string.h"
#include "sentry_sync.h"
#include "sentry_transport.h"
#include "sentry_value.h"

#ifdef SENTRY_INTEGRATION_QT
#    include "integrations/sentry_integration_qt.h"
#endif

static sentry_options_t *g_options = NULL;
static sentry_mutex_t g_options_lock = SENTRY__MUTEX_INIT;

const sentry_options_t *
sentry__options_getref(void)
{
    sentry_options_t *options;
    sentry__mutex_lock(&g_options_lock);
    options = sentry__options_incref(g_options);
    sentry__mutex_unlock(&g_options_lock);
    return options;
}

static void
load_user_consent(sentry_options_t *opts)
{
    sentry_path_t *consent_path
        = sentry__path_join_str(opts->database_path, "user-consent");
    char *contents = sentry__path_read_to_buffer(consent_path, NULL);
    sentry__path_free(consent_path);
    switch (contents ? contents[0] : 0) {
    case '1':
        opts->user_consent = SENTRY_USER_CONSENT_GIVEN;
        break;
    case '0':
        opts->user_consent = SENTRY_USER_CONSENT_REVOKED;
        break;
    default:
        opts->user_consent = SENTRY_USER_CONSENT_UNKNOWN;
        break;
    }
    sentry_free(contents);
}

bool
sentry__should_skip_upload(void)
{
    bool skip = true;
    SENTRY_WITH_OPTIONS (options) {
        skip = options->require_user_consent
            && sentry__atomic_fetch((long *)&options->user_consent)
                != SENTRY_USER_CONSENT_GIVEN;
    }
    return skip;
}

int
sentry_init(sentry_options_t *options)
{
    sentry_shutdown();

    sentry_logger_t logger = { NULL, NULL };
    if (options->debug) {
        logger = options->logger;
    }
    sentry__logger_set_global(logger);

    // we need to ensure the dir exists, otherwise `path_absolute` will fail.
    if (sentry__path_create_dir_all(options->database_path)) {
        SENTRY_WARN("failed to create database directory or there is no write "
                    "access to this directory");
        sentry_options_free(options);
        return 1;
    }
    sentry_transport_t *transport = options->transport;

    sentry_path_t *database_path = options->database_path;
    options->database_path = sentry__path_absolute(database_path);
    if (options->database_path) {
        sentry__path_free(database_path);
    } else {
        SENTRY_DEBUG("falling back to non-absolute database path");
        options->database_path = database_path;
    }
    SENTRY_DEBUGF("using database path \"%" SENTRY_PATH_PRI "\"",
        options->database_path->path);

    // try to create and lock our run folder as early as possibly, since it is
    // fallible. since it does locking, it will not interfere with run folder
    // enumeration.
    options->run = sentry__run_new(options->database_path);
    if (!options->run) {
        SENTRY_WARN("failed to initialize run directory");
        goto fail;
    }

    load_user_consent(options);

    if (!options->dsn || !options->dsn->is_valid) {
        const char *raw_dsn = sentry_options_get_dsn(options);
        SENTRY_WARNF(
            "the provided DSN \"%s\" is not valid", raw_dsn ? raw_dsn : "");
    }

    if (transport) {
        if (sentry__transport_startup(transport, options) != 0) {
            SENTRY_WARN("failed to initialize transport");
            goto fail;
        }
    }

    uint64_t last_crash = 0;

    // and then we will start the backend, since it requires a valid run
    sentry_backend_t *backend = options->backend;
    if (backend && backend->startup_func) {
        SENTRY_TRACE("starting backend");
        if (backend->startup_func(backend, options) != 0) {
            SENTRY_WARN("failed to initialize backend");
            goto fail;
        }
    }
    if (backend && backend->get_last_crash_func) {
        last_crash = backend->get_last_crash_func(backend);
    }

    sentry__mutex_lock(&g_options_lock);
    g_options = options;
    sentry__mutex_unlock(&g_options_lock);

    // *after* setting the global options, trigger a scope and consent flush,
    // since at least crashpad needs that.
    // the only way to get a reference to the scope is by locking it, the macro
    // does all that at once, including invoking the backends scope flush hook
    SENTRY_WITH_SCOPE_MUT (scope) {
        (void)scope;
    }
    if (backend && backend->user_consent_changed_func) {
        backend->user_consent_changed_func(backend);
    }

#ifdef SENTRY_INTEGRATION_QT
    SENTRY_TRACE("setting up Qt integration");
    sentry_integration_setup_qt();
#endif

    // after initializing the transport, we will submit all the unsent envelopes
    // and handle remaining sessions.
    sentry__process_old_runs(options, last_crash);

    if (options->auto_session_tracking) {
        sentry_start_session();
    }

    return 0;

fail:
    SENTRY_WARN("`sentry_init` failed");
    if (transport) {
        sentry__transport_shutdown(transport, 0);
    }
    sentry_options_free(options);
    return 1;
}

int
sentry_shutdown(void)
{
    sentry_end_session();

    sentry__mutex_lock(&g_options_lock);
    sentry_options_t *options = g_options;
    g_options = NULL;
    sentry__mutex_unlock(&g_options_lock);

    size_t dumped_envelopes = 0;
    if (options) {
        if (options->backend && options->backend->shutdown_func) {
            SENTRY_TRACE("shutting down backend");
            options->backend->shutdown_func(options->backend);
        }

        if (options->transport) {
            // TODO: make this configurable
            if (sentry__transport_shutdown(
                    options->transport, SENTRY_DEFAULT_SHUTDOWN_TIMEOUT)
                != 0) {
                SENTRY_WARN("transport did not shut down cleanly");
            }
            dumped_envelopes = sentry__transport_dump_queue(
                options->transport, options->run);
        }
        if (!dumped_envelopes
            && (!options->backend
                || !options->backend->can_capture_after_shutdown)) {
            sentry__run_clean(options->run);
        }

        sentry_options_free(options);
    }

    sentry__scope_cleanup();
    sentry_clear_modulecache();
    return (int)dumped_envelopes;
}

int
sentry_reinstall_backend(void)
{
    int rv = 0;
    SENTRY_WITH_OPTIONS (options) {
        sentry_backend_t *backend = options->backend;
        if (backend && backend->shutdown_func) {
            backend->shutdown_func(backend);
        }

        if (backend && backend->startup_func) {
            if (backend->startup_func(backend, options)) {
                rv = 1;
            }
        }
    }
    return rv;
}

static void
set_user_consent(sentry_user_consent_t new_val)
{
    SENTRY_WITH_OPTIONS (options) {
        if (sentry__atomic_store((long *)&options->user_consent, new_val)
            == new_val) {
            // nothing was changed
            break; // SENTRY_WITH_OPTIONS
        }

        if (options->backend && options->backend->user_consent_changed_func) {
            options->backend->user_consent_changed_func(options->backend);
        }

        sentry_path_t *consent_path
            = sentry__path_join_str(options->database_path, "user-consent");
        switch (new_val) {
        case SENTRY_USER_CONSENT_GIVEN:
            sentry__path_write_buffer(consent_path, "1\n", 2);
            break;
        case SENTRY_USER_CONSENT_REVOKED:
            sentry__path_write_buffer(consent_path, "0\n", 2);
            break;
        case SENTRY_USER_CONSENT_UNKNOWN:
            sentry__path_remove(consent_path);
            break;
        }
        sentry__path_free(consent_path);
    }
}

void
sentry_user_consent_give(void)
{
    set_user_consent(SENTRY_USER_CONSENT_GIVEN);
}

void
sentry_user_consent_revoke(void)
{
    set_user_consent(SENTRY_USER_CONSENT_REVOKED);
}

void
sentry_user_consent_reset(void)
{
    set_user_consent(SENTRY_USER_CONSENT_UNKNOWN);
}

sentry_user_consent_t
sentry_user_consent_get(void)
{
    sentry_user_consent_t rv = SENTRY_USER_CONSENT_UNKNOWN;
    SENTRY_WITH_OPTIONS (options) {
        rv = (sentry_user_consent_t)sentry__atomic_fetch(
            (long *)&options->user_consent);
    }
    return rv;
}

void
sentry__capture_envelope(
    sentry_transport_t *transport, sentry_envelope_t *envelope)
{
    bool has_consent = !sentry__should_skip_upload();
    if (!has_consent) {
        SENTRY_TRACE("discarding envelope due to missing user consent");
        sentry_envelope_free(envelope);
        return;
    }
    sentry__transport_send_envelope(transport, envelope);
}

static bool
event_is_considered_error(sentry_value_t event)
{
    const char *level
        = sentry_value_as_string(sentry_value_get_by_key(event, "level"));
    if (sentry__string_eq(level, "fatal")
        || sentry__string_eq(level, "error")) {
        return true;
    }
    if (!sentry_value_is_null(sentry_value_get_by_key(event, "exception"))) {
        return true;
    }
    return false;
}

sentry_uuid_t
sentry_capture_event(sentry_value_t event)
{
    sentry_uuid_t event_id;
    sentry_envelope_t *envelope = NULL;

    bool was_captured = false;
    SENTRY_WITH_OPTIONS (options) {
        was_captured = true;
        envelope = sentry__prepare_event(options, event, &event_id);
        if (envelope) {
            sentry__add_current_session_to_envelope(envelope);
            sentry__capture_envelope(options->transport, envelope);
        }
    }
    if (!was_captured) {
        sentry_value_decref(event);
    }
    return was_captured ? event_id : sentry_uuid_nil();
}

sentry_envelope_t *
sentry__prepare_event(const sentry_options_t *options, sentry_value_t event,
    sentry_uuid_t *event_id)
{
    sentry_envelope_t *envelope = NULL;

    if (event_is_considered_error(event)) {
        sentry__record_errors_on_current_session(1);
    }

    uint64_t rnd;
    if (options->sample_rate < 1.0 && !sentry__getrandom(&rnd, sizeof(rnd))
        && ((double)rnd / (double)UINT64_MAX) > options->sample_rate) {
        SENTRY_DEBUG("throwing away event due to sample rate");
        goto fail;
    }

    SENTRY_WITH_SCOPE (scope) {
        SENTRY_TRACE("merging scope into event");
        sentry_scope_mode_t mode = SENTRY_SCOPE_ALL;
        if (!options->symbolize_stacktraces) {
            mode &= ~SENTRY_SCOPE_STACKTRACES;
        }
        sentry__scope_apply_to_event(scope, event, mode);
    }

    if (options->before_send_func) {
        SENTRY_TRACE("invoking `before_send` hook");
        event
            = options->before_send_func(event, NULL, options->before_send_data);
        if (sentry_value_is_null(event)) {
            SENTRY_TRACE("event was discarded by the `before_send` hook");
            return NULL;
        }
    }

    sentry__ensure_event_id(event, event_id);
    envelope = sentry__envelope_new();
    if (!envelope || !sentry__envelope_add_event(envelope, event)) {
        goto fail;
    }

    SENTRY_TRACE("adding attachments to envelope");
    for (sentry_attachment_t *attachment = options->attachments; attachment;
         attachment = attachment->next) {
        sentry_envelope_item_t *item = sentry__envelope_add_from_path(
            envelope, attachment->path, "attachment");
        if (!item) {
            continue;
        }
        sentry__envelope_item_set_header(item, "filename",
#ifdef SENTRY_PLATFORM_WINDOWS
            sentry__value_new_string_from_wstr(
#else
            sentry_value_new_string(
#endif
                sentry__path_filename(attachment->path)));
    }

    return envelope;

fail:
    sentry_envelope_free(envelope);
    sentry_value_decref(event);
    return NULL;
}

void
sentry_handle_exception(const sentry_ucontext_t *uctx)
{
    SENTRY_WITH_OPTIONS (options) {
        SENTRY_DEBUG("handling exception");
        if (options->backend && options->backend->except_func) {
            options->backend->except_func(options->backend, uctx);
        }
    }
}

sentry_uuid_t
sentry__new_event_id(void)
{
#if SENTRY_UNITTEST
    return sentry_uuid_from_string("4c035723-8638-4c3a-923f-2ab9d08b4018");
#else
    return sentry_uuid_new_v4();
#endif
}

sentry_value_t
sentry__ensure_event_id(sentry_value_t event, sentry_uuid_t *uuid_out)
{
    sentry_value_t event_id = sentry_value_get_by_key(event, "event_id");
    sentry_uuid_t uuid = sentry__value_as_uuid(event_id);
    if (sentry_uuid_is_nil(&uuid)) {
        uuid = sentry__new_event_id();
        event_id = sentry__value_new_uuid(&uuid);
        sentry_value_set_by_key(event, "event_id", event_id);
    }
    if (uuid_out) {
        *uuid_out = uuid;
    }
    return event_id;
}

void
sentry_set_user(sentry_value_t user)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_decref(scope->user);
        scope->user = user;
        sentry__scope_session_sync(scope);
    }
}

void
sentry_remove_user(void)
{
    sentry_set_user(sentry_value_new_null());
}

void
sentry_add_breadcrumb(sentry_value_t breadcrumb)
{
    size_t max_breadcrumbs = SENTRY_BREADCRUMBS_MAX;
    SENTRY_WITH_OPTIONS (options) {
        if (options->backend && options->backend->add_breadcrumb_func) {
            // the hook will *not* take ownership
            options->backend->add_breadcrumb_func(options->backend, breadcrumb);
        }
        max_breadcrumbs = options->max_breadcrumbs;
    }

    // the `no_flush` will avoid triggering *both* scope-change and
    // breadcrumb-add events.
    SENTRY_WITH_SCOPE_MUT_NO_FLUSH (scope) {
        sentry__value_append_bounded(
            scope->breadcrumbs, breadcrumb, max_breadcrumbs);
    }
}

void
sentry_set_tag(const char *key, const char *value)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_set_by_key(
            scope->tags, key, sentry_value_new_string(value));
    }
}

void
sentry_remove_tag(const char *key)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_remove_by_key(scope->tags, key);
    }
}

void
sentry_set_extra(const char *key, sentry_value_t value)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_set_by_key(scope->extra, key, value);
    }
}

void
sentry_remove_extra(const char *key)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_remove_by_key(scope->extra, key);
    }
}

void
sentry_set_context(const char *key, sentry_value_t value)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_set_by_key(scope->contexts, key, value);
    }
}

void
sentry_remove_context(const char *key)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_remove_by_key(scope->contexts, key);
    }
}

void
sentry_set_fingerprint(const char *fingerprint, ...)
{
    sentry_value_t fingerprint_value = sentry_value_new_list();

    va_list va;
    va_start(va, fingerprint);
    for (; fingerprint; fingerprint = va_arg(va, const char *)) {
        sentry_value_append(
            fingerprint_value, sentry_value_new_string(fingerprint));
    }
    va_end(va);

    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_decref(scope->fingerprint);
        scope->fingerprint = fingerprint_value;
    };
}

void
sentry_remove_fingerprint(void)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_decref(scope->fingerprint);
        scope->fingerprint = sentry_value_new_null();
    };
}

void
sentry_set_transaction(const char *transaction)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_free(scope->transaction);
        scope->transaction = sentry__string_clone(transaction);
    }
}

void
sentry_remove_transaction(void)
{
    sentry_set_transaction(NULL);
}

void
sentry_set_level(sentry_level_t level)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        scope->level = level;
    }
}
