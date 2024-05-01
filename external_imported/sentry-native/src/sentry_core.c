#include "sentry_boot.h"

#include <stdarg.h>
#include <string.h>

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
#include "sentry_tracing.h"
#include "sentry_transport.h"
#include "sentry_value.h"

#ifdef SENTRY_INTEGRATION_QT
#    include "integrations/sentry_integration_qt.h"
#endif

static sentry_options_t *g_options = NULL;
static sentry_mutex_t g_options_lock = SENTRY__MUTEX_INIT;

/// see sentry_get_crashed_last_run() for the possible values
static int g_last_crash = -1;

const sentry_options_t *
sentry__options_getref(void)
{
    sentry_options_t *options;
    sentry__mutex_lock(&g_options_lock);
    options = sentry__options_incref(g_options);
    sentry__mutex_unlock(&g_options_lock);
    return options;
}

sentry_options_t *
sentry__options_lock(void)
{
    sentry__mutex_lock(&g_options_lock);
    return g_options;
}

void
sentry__options_unlock(void)
{
    sentry__mutex_unlock(&g_options_lock);
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
    // this function is to be called only once, so we do not allow more than one
    // caller
    sentry__mutex_lock(&g_options_lock);
    // pre-init here, so we can consistently use bailing out to :fail
    sentry_transport_t *transport = NULL;

    sentry_close();

    sentry_logger_t logger = { NULL, NULL };
    if (options->debug) {
        logger = options->logger;
    }
    sentry__logger_set_global(logger);

    // we need to ensure the dir exists, otherwise `path_absolute` will fail.
    if (sentry__path_create_dir_all(options->database_path)) {
        SENTRY_WARN("failed to create database directory or there is no write "
                    "access to this directory");
        goto fail;
    }

    transport = options->transport;
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

    g_last_crash = sentry__has_crash_marker(options);
    g_options = options;

    // *after* setting the global options, trigger a scope and consent flush,
    // since at least crashpad needs that. At this point we also freeze the
    // `client_sdk` in the `scope` because some downstream SDKs want to override
    // it at runtime via the options interface.
    SENTRY_WITH_SCOPE_MUT (scope) {
        if (options->sdk_name) {
            sentry_value_t sdk_name
                = sentry_value_new_string(options->sdk_name);
            sentry_value_set_by_key(scope->client_sdk, "name", sdk_name);
        }
        sentry_value_freeze(scope->client_sdk);
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
    SENTRY_TRACE("processing and pruning old runs");
    sentry__process_old_runs(options, last_crash);
    if (backend && backend->prune_database_func) {
        backend->prune_database_func(backend);
    }

    if (options->auto_session_tracking) {
        sentry_start_session();
    }

    sentry__mutex_unlock(&g_options_lock);
    return 0;

fail:
    SENTRY_WARN("`sentry_init` failed");
    if (transport) {
        sentry__transport_shutdown(transport, 0);
    }
    sentry_options_free(options);
    sentry__mutex_unlock(&g_options_lock);
    return 1;
}

int
sentry_flush(uint64_t timeout)
{
    int rv = 0;
    SENTRY_WITH_OPTIONS (options) {
        rv = sentry__transport_flush(options->transport, timeout);
    }
    return rv;
}

int
sentry_close(void)
{
    // this function is to be called only once, so we do not allow more than one
    // caller
    sentry__mutex_lock(&g_options_lock);
    sentry_options_t *options = g_options;

    size_t dumped_envelopes = 0;
    if (options) {
        sentry_end_session();
        if (options->backend && options->backend->shutdown_func) {
            SENTRY_TRACE("shutting down backend");
            options->backend->shutdown_func(options->backend);
        }

        if (options->transport) {
            if (sentry__transport_shutdown(
                    options->transport, options->shutdown_timeout)
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
    } else {
        SENTRY_DEBUG("sentry_close() called, but options was empty");
    }

    g_options = NULL;
    sentry__mutex_unlock(&g_options_lock);

    sentry__scope_cleanup();
    sentry_clear_modulecache();

    return (int)dumped_envelopes;
}

int
sentry_shutdown(void)
{
    return sentry_close();
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
            != new_val) {
            if (options->backend
                && options->backend->user_consent_changed_func) {
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

bool
sentry__event_is_transaction(sentry_value_t event)
{
    sentry_value_t event_type = sentry_value_get_by_key(event, "type");
    return sentry__string_eq("transaction", sentry_value_as_string(event_type));
}

sentry_uuid_t
sentry_capture_event(sentry_value_t event)
{
    if (sentry__event_is_transaction(event)) {
        return sentry_uuid_nil();
    } else {
        return sentry__capture_event(event);
    }
}

bool
sentry__roll_dice(double probability)
{
    uint64_t rnd;
    return probability >= 1.0 || sentry__getrandom(&rnd, sizeof(rnd))
        || ((double)rnd / (double)UINT64_MAX) <= probability;
}

sentry_uuid_t
sentry__capture_event(sentry_value_t event)
{
    sentry_uuid_t event_id;
    sentry_envelope_t *envelope = NULL;

    bool was_captured = false;
    bool was_sent = false;
    SENTRY_WITH_OPTIONS (options) {
        was_captured = true;

        if (sentry__event_is_transaction(event)) {
            envelope = sentry__prepare_transaction(options, event, &event_id);
        } else {
            envelope = sentry__prepare_event(options, event, &event_id, true);
        }
        if (envelope) {
            if (options->session) {
                sentry_options_t *mut_options = sentry__options_lock();
                sentry__envelope_add_session(envelope, mut_options->session);
                // we're assuming that if a session is added to an envelope
                // it will be sent onwards.  This means we now need to set
                // the init flag to false because we're no longer the
                // initial session update.
                mut_options->session->init = false;
                sentry__options_unlock();
            }

            bool should_skip = !sentry__roll_dice(options->sample_rate);
            if (should_skip) {
                SENTRY_DEBUG("throwing away event due to sample rate");
                sentry_envelope_free(envelope);
            } else {
                sentry__capture_envelope(options->transport, envelope);
                was_sent = true;
            }
        }
    }
    if (!was_captured) {
        sentry_value_decref(event);
    }
    return was_sent ? event_id : sentry_uuid_nil();
}

bool
sentry__should_send_transaction(sentry_value_t tx_cxt)
{
    sentry_value_t context_setting = sentry_value_get_by_key(tx_cxt, "sampled");
    if (!sentry_value_is_null(context_setting)) {
        return sentry_value_is_true(context_setting);
    }

    bool send = false;
    SENTRY_WITH_OPTIONS (options) {
        send = sentry__roll_dice(options->traces_sample_rate);
        // TODO(tracing): Run through traces sampler function if rate is
        // unavailable.
    }
    return send;
}

sentry_envelope_t *
sentry__prepare_event(const sentry_options_t *options, sentry_value_t event,
    sentry_uuid_t *event_id, bool invoke_before_send)
{
    sentry_envelope_t *envelope = NULL;

    if (event_is_considered_error(event)) {
        sentry__record_errors_on_current_session(1);
    }

    SENTRY_WITH_SCOPE (scope) {
        SENTRY_TRACE("merging scope into event");
        sentry_scope_mode_t mode = SENTRY_SCOPE_ALL;
        if (!options->symbolize_stacktraces) {
            mode &= ~SENTRY_SCOPE_STACKTRACES;
        }
        sentry__scope_apply_to_event(scope, options, event, mode);
    }

    if (options->before_send_func && invoke_before_send) {
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

sentry_envelope_t *
sentry__prepare_transaction(const sentry_options_t *options,
    sentry_value_t transaction, sentry_uuid_t *event_id)
{
    sentry_envelope_t *envelope = NULL;

    SENTRY_WITH_SCOPE (scope) {
        SENTRY_TRACE("merging scope into transaction");
        // Don't include debugging info
        sentry_scope_mode_t mode = SENTRY_SCOPE_ALL & ~SENTRY_SCOPE_MODULES
            & ~SENTRY_SCOPE_STACKTRACES;
        sentry__scope_apply_to_event(scope, options, transaction, mode);
    }

    sentry__ensure_event_id(transaction, event_id);
    envelope = sentry__envelope_new();
    if (!envelope || !sentry__envelope_add_transaction(envelope, transaction)) {
        goto fail;
    }

    // TODO(tracing): Revisit when adding attachment support for transactions.

    return envelope;

fail:
    SENTRY_WARN("dropping transaction");
    sentry_envelope_free(envelope);
    sentry_value_decref(transaction);
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
#ifdef SENTRY_UNITTEST
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
    if (!sentry_value_is_null(user)) {
        sentry_options_t *options = sentry__options_lock();
        if (options && options->session) {
            sentry__session_sync_user(options->session, user);
            sentry__run_write_session(options->run, options->session);
        }
        sentry__options_unlock();
    }

    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_decref(scope->user);
        scope->user = user;
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
            options->backend->add_breadcrumb_func(
                options->backend, breadcrumb, options);
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
sentry_set_tag_n(
    const char *key, size_t key_len, const char *value, size_t value_len)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_set_by_key_n(scope->tags, key, key_len,
            sentry_value_new_string_n(value, value_len));
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
sentry_remove_tag_n(const char *key, size_t key_len)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_remove_by_key_n(scope->tags, key, key_len);
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
sentry_set_extra_n(const char *key, size_t key_len, sentry_value_t value)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_set_by_key_n(scope->extra, key, key_len, value);
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
sentry_remove_extra_n(const char *key, size_t key_len)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_remove_by_key_n(scope->extra, key, key_len);
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
sentry_set_context_n(const char *key, size_t key_len, sentry_value_t value)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_set_by_key_n(scope->contexts, key, key_len, value);
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
sentry_remove_context_n(const char *key, size_t key_len)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_remove_by_key_n(scope->contexts, key, key_len);
    }
}

void
sentry_set_fingerprint_n(const char *fingerprint, size_t fingerprint_len, ...)
{
    sentry_value_t fingerprint_value = sentry_value_new_list();

    va_list va;
    va_start(va, fingerprint_len);
    for (; fingerprint; fingerprint = va_arg(va, const char *)) {
        sentry_value_append(fingerprint_value,
            sentry_value_new_string_n(fingerprint, fingerprint_len));
    }
    va_end(va);

    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_decref(scope->fingerprint);
        scope->fingerprint = fingerprint_value;
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
    }
}

void
sentry_remove_fingerprint(void)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_decref(scope->fingerprint);
        scope->fingerprint = sentry_value_new_null();
    }
}

void
sentry_set_transaction(const char *transaction)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_free(scope->transaction);
        scope->transaction = sentry__string_clone(transaction);

        if (scope->transaction_object) {
            sentry_transaction_set_name(scope->transaction_object, transaction);
        }
    }
}

void
sentry_set_transaction_n(const char *transaction, size_t transaction_len)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_free(scope->transaction);
        scope->transaction
            = sentry__string_clone_n(transaction, transaction_len);

        if (scope->transaction_object) {
            sentry_transaction_set_name_n(
                scope->transaction_object, transaction, transaction_len);
        }
    }
}

void
sentry_set_level(sentry_level_t level)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        scope->level = level;
    }
}

sentry_transaction_t *
sentry_transaction_start(
    sentry_transaction_context_t *opaque_tx_cxt, sentry_value_t sampling_ctx)
{
    // Just free this immediately until we implement proper support for
    // traces_sampler.
    sentry_value_decref(sampling_ctx);

    if (!opaque_tx_cxt) {
        return NULL;
    }

    sentry_value_t tx_cxt = opaque_tx_cxt->inner;

    // If the parent span ID is some empty-ish value, just remove it
    sentry_value_t parent_span
        = sentry_value_get_by_key(tx_cxt, "parent_span_id");
    if (sentry_value_get_length(parent_span) < 1) {
        sentry_value_remove_by_key(tx_cxt, "parent_span_id");
    }

    // The ending timestamp is stripped to avoid misleading ourselves later
    // down the line, as it is the only way to determine whether a transaction
    // has ended or not.
    sentry_value_t tx = sentry_value_new_event();
    sentry_value_remove_by_key(tx, "timestamp");

    sentry__value_merge_objects(tx, tx_cxt);

    bool should_sample = sentry__should_send_transaction(tx_cxt);
    sentry_value_set_by_key(
        tx, "sampled", sentry_value_new_bool(should_sample));

    sentry_value_set_by_key(tx, "start_timestamp",
        sentry__value_new_string_owned(
            sentry__msec_time_to_iso8601(sentry__msec_time())));

    sentry__transaction_context_free(opaque_tx_cxt);
    return sentry__transaction_new(tx);
}

sentry_uuid_t
sentry_transaction_finish(sentry_transaction_t *opaque_tx)
{
    if (!opaque_tx || sentry_value_is_null(opaque_tx->inner)) {
        SENTRY_DEBUG("no transaction available to finish");
        goto fail;
    }

    sentry_value_t tx = sentry__value_clone(opaque_tx->inner);

    SENTRY_WITH_SCOPE_MUT (scope) {
        if (scope->transaction_object) {
            sentry_value_t scope_tx = scope->transaction_object->inner;

            const char *tx_id = sentry_value_as_string(
                sentry_value_get_by_key(tx, "span_id"));
            const char *scope_tx_id = sentry_value_as_string(
                sentry_value_get_by_key(scope_tx, "span_id"));
            if (sentry__string_eq(tx_id, scope_tx_id)) {
                sentry__transaction_decref(scope->transaction_object);
                scope->transaction_object = NULL;
            }
        }
    }
    // The sampling decision should already be made for transactions
    // during their construction. No need to recalculate here. See
    // `sentry__should_skip_transaction`.
    sentry_value_t sampled = sentry_value_get_by_key(tx, "sampled");
    if (!sentry_value_is_true(sampled)) {
        SENTRY_DEBUG("throwing away transaction due to sample rate or "
                     "user-provided sampling value in transaction context");
        sentry_value_decref(tx);
        goto fail;
    }
    sentry_value_remove_by_key(tx, "sampled");

    sentry_value_set_by_key(tx, "type", sentry_value_new_string("transaction"));
    sentry_value_set_by_key(tx, "timestamp",
        sentry__value_new_string_owned(
            sentry__msec_time_to_iso8601(sentry__msec_time())));
    // TODO: This might not actually be necessary. Revisit after talking to
    // the relay team about this.
    sentry_value_set_by_key(tx, "level", sentry_value_new_string("info"));

    sentry_value_t name = sentry_value_get_by_key(tx, "transaction");
    if (sentry_value_is_null(name) || sentry_value_get_length(name) == 0) {
        sentry_value_set_by_key(tx, "transaction",
            sentry_value_new_string("<unlabeled transaction>"));
    }

    // TODO: add tracestate
    sentry_value_t trace_context
        = sentry__value_get_trace_context(opaque_tx->inner);
    sentry_value_t contexts = sentry_value_new_object();
    sentry_value_set_by_key(contexts, "trace", trace_context);
    sentry_value_set_by_key(tx, "contexts", contexts);

    // clean up trace context fields
    sentry_value_remove_by_key(tx, "trace_id");
    sentry_value_remove_by_key(tx, "span_id");
    sentry_value_remove_by_key(tx, "parent_span_id");
    sentry_value_remove_by_key(tx, "op");
    sentry_value_remove_by_key(tx, "description");
    sentry_value_remove_by_key(tx, "status");

    sentry__transaction_decref(opaque_tx);

    // This takes ownership of the transaction, generates an event ID, merges
    // scope
    return sentry__capture_event(tx);
fail:
    sentry__transaction_decref(opaque_tx);
    return sentry_uuid_nil();
}

void
sentry_set_transaction_object(sentry_transaction_t *tx)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry__span_decref(scope->span);
        scope->span = NULL;
        sentry__transaction_decref(scope->transaction_object);
        sentry__transaction_incref(tx);
        scope->transaction_object = tx;
    }
}

void
sentry_set_span(sentry_span_t *span)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry__transaction_decref(scope->transaction_object);
        scope->transaction_object = NULL;
        sentry__span_decref(scope->span);
        sentry__span_incref(span);
        scope->span = span;
    }
}

sentry_span_t *
sentry_transaction_start_child_n(sentry_transaction_t *opaque_parent,
    const char *operation, size_t operation_len, const char *description,
    size_t description_len)
{
    if (!opaque_parent || sentry_value_is_null(opaque_parent->inner)) {
        SENTRY_DEBUG("no transaction available to create a child under");
        return NULL;
    }
    sentry_value_t parent = opaque_parent->inner;

    // TODO: consider snapshotting this value during tx creation and storing in
    // tx and span
    size_t max_spans = SENTRY_SPANS_MAX;
    SENTRY_WITH_OPTIONS (options) {
        max_spans = options->max_spans;
    }

    sentry_value_t span = sentry__value_span_new_n(max_spans, parent,
        (sentry_slice_t) { operation, operation_len },
        (sentry_slice_t) { description, description_len });
    return sentry__span_new(opaque_parent, span);
}

sentry_span_t *
sentry_transaction_start_child(sentry_transaction_t *opaque_parent,
    const char *operation, const char *description)
{
    const size_t operation_len = operation ? strlen(operation) : 0;
    const size_t description_len = description ? strlen(description) : 0;
    return sentry_transaction_start_child_n(
        opaque_parent, operation, operation_len, description, description_len);
}

sentry_span_t *
sentry_span_start_child_n(sentry_span_t *opaque_parent, const char *operation,
    size_t operation_len, const char *description, size_t description_len)
{
    if (!opaque_parent || sentry_value_is_null(opaque_parent->inner)) {
        SENTRY_DEBUG("no parent span available to create a child span under");
        return NULL;
    }
    if (!opaque_parent->transaction) {
        SENTRY_DEBUG("no root transaction to create a child span under");
        return NULL;
    }
    sentry_value_t parent = opaque_parent->inner;

    // TODO: consider snapshotting this value during tx creation and storing in
    // tx and span
    size_t max_spans = SENTRY_SPANS_MAX;
    SENTRY_WITH_OPTIONS (options) {
        max_spans = options->max_spans;
    }

    sentry_value_t span = sentry__value_span_new_n(max_spans, parent,
        (sentry_slice_t) { operation, operation_len },
        (sentry_slice_t) { description, description_len });

    return sentry__span_new(opaque_parent->transaction, span);
}

sentry_span_t *
sentry_span_start_child(sentry_span_t *opaque_parent, const char *operation,
    const char *description)
{
    size_t operation_len = operation ? strlen(operation) : 0;
    size_t description_len = description ? strlen(description) : 0;
    return sentry_span_start_child_n(
        opaque_parent, operation, operation_len, description, description_len);
}

void
sentry_span_finish(sentry_span_t *opaque_span)
{
    if (!opaque_span || sentry_value_is_null(opaque_span->inner)) {
        SENTRY_DEBUG("no span to finish");
        goto fail;
    }

    sentry_transaction_t *opaque_root_transaction = opaque_span->transaction;
    if (!opaque_root_transaction
        || sentry_value_is_null(opaque_root_transaction->inner)) {
        SENTRY_DEBUG(
            "no root transaction to finish span on, aborting span finish");
        goto fail;
    }

    sentry_value_t root_transaction = opaque_root_transaction->inner;

    if (!sentry_value_is_true(
            sentry_value_get_by_key(root_transaction, "sampled"))) {
        SENTRY_DEBUG("root transaction is unsampled, dropping span");
        goto fail;
    }

    if (!sentry_value_is_null(
            sentry_value_get_by_key(root_transaction, "timestamp"))) {
        SENTRY_DEBUG("span's root transaction is already finished, aborting "
                     "span finish");
        goto fail;
    }

    sentry_value_t span = sentry__value_clone(opaque_span->inner);

    SENTRY_WITH_SCOPE_MUT (scope) {
        if (scope->span) {
            sentry_value_t scope_span = scope->span->inner;

            const char *span_id = sentry_value_as_string(
                sentry_value_get_by_key(span, "span_id"));
            const char *scope_span_id = sentry_value_as_string(
                sentry_value_get_by_key(scope_span, "span_id"));
            if (sentry__string_eq(span_id, scope_span_id)) {
                sentry__span_decref(scope->span);
                scope->span = NULL;
            }
        }
    }

    // Note that the current API makes it impossible to set a sampled value
    // that's different from the span's root transaction, but let's just be safe
    // here.
    if (!sentry_value_is_true(sentry_value_get_by_key(span, "sampled"))) {
        SENTRY_DEBUG("span is unsampled, dropping span");
        sentry_value_decref(span);
        goto fail;
    }

    if (!sentry_value_is_null(sentry_value_get_by_key(span, "timestamp"))) {
        SENTRY_DEBUG("span is already finished, aborting span finish");
        sentry_value_decref(span);
        goto fail;
    }

    sentry_value_set_by_key(span, "timestamp",
        sentry__value_new_string_owned(
            sentry__msec_time_to_iso8601(sentry__msec_time())));
    sentry_value_remove_by_key(span, "sampled");

    size_t max_spans = SENTRY_SPANS_MAX;
    SENTRY_WITH_OPTIONS (options) {
        max_spans = options->max_spans;
    }

    sentry_value_t spans = sentry_value_get_by_key(root_transaction, "spans");

    if (sentry_value_get_length(spans) >= max_spans) {
        SENTRY_DEBUG("reached maximum number of spans for transaction, "
                     "discarding span");
        sentry_value_decref(span);
        goto fail;
    }

    if (sentry_value_is_null(spans)) {
        spans = sentry_value_new_list();
        sentry_value_set_by_key(root_transaction, "spans", spans);
    }
    sentry_value_append(spans, span);
    sentry__span_decref(opaque_span);
    return;

fail:
    sentry__span_decref(opaque_span);
}

int
sentry_get_crashed_last_run(void)
{
    return g_last_crash;
}

int
sentry_clear_crashed_last_run(void)
{
    bool success = false;
    sentry_options_t *options = sentry__options_lock();
    if (options) {
        success = sentry__clear_crash_marker(options);
    }
    sentry__options_unlock();
    return success ? 0 : 1;
}
