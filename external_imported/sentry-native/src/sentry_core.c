#include "sentry_boot.h"

#include <stdarg.h>
#include <string.h>

#include "sentry_app_hang_latch.h"
#include "sentry_app_hang_monitor.h"
#include "sentry_attachment.h"
#include "sentry_backend.h"
#include "sentry_client_report.h"
#include "sentry_core.h"
#include "sentry_database.h"
#include "sentry_envelope.h"
#include "sentry_hint.h"
#include "sentry_logs.h"
#include "sentry_metrics.h"
#include "sentry_options.h"
#include "sentry_path.h"
#include "sentry_process.h"
#include "sentry_random.h"
#include "sentry_scope.h"
#include "sentry_session.h"
#include "sentry_string.h"
#include "sentry_sync.h"
#include "sentry_tracing.h"
#include "sentry_transport.h"
#include "sentry_tsan.h"
#include "sentry_uuid.h"
#include "sentry_value.h"
#include "transports/sentry_disk_transport.h"

#ifdef SENTRY_PLATFORM_WINDOWS
#    include "sentry_os.h"
#    include "sentry_screenshot.h"
#endif

#ifdef SENTRY_INTEGRATION_QT
#    include "integrations/sentry_integration_qt.h"
#endif

static sentry_options_t *g_options = NULL;
#ifdef SENTRY__MUTEX_INIT_DYN
SENTRY__MUTEX_INIT_DYN(g_options_lock)
#else
static sentry_mutex_t g_options_lock = SENTRY__MUTEX_INIT;
#endif
/// see sentry_get_crashed_last_run() for the possible values
static int g_last_crash = -1;

const sentry_options_t *
sentry__options_getref(void)
{
    SENTRY__MUTEX_INIT_DYN_ONCE(g_options_lock);
    sentry_options_t *options;
    sentry__mutex_lock(&g_options_lock);
    options = sentry__options_incref(g_options);
    sentry__mutex_unlock(&g_options_lock);
    return options;
}

sentry_options_t *
sentry__options_lock(void)
{
    SENTRY__MUTEX_INIT_DYN_ONCE(g_options_lock);
    sentry__mutex_lock(&g_options_lock);
    return g_options;
}

void
sentry__options_unlock(void)
{
    SENTRY__MUTEX_INIT_DYN_ONCE(g_options_lock);
    sentry__mutex_unlock(&g_options_lock);
}

bool
sentry__should_skip_upload(void)
{
    bool skip = true;
    SENTRY_WITH_OPTIONS (options) {
        skip = sentry__run_should_skip_upload(options->run);
    }
    return skip;
}

static void
generate_propagation_context(sentry_value_t propagation_context)
{
    sentry_value_set_by_key(
        propagation_context, "trace", sentry_value_new_object());
    sentry_uuid_t trace_id = sentry_uuid_new_v4();
    sentry_uuid_t span_id = sentry_uuid_new_v4();
    sentry_value_set_by_key(
        sentry_value_get_by_key(propagation_context, "trace"), "trace_id",
        sentry__value_new_internal_uuid(&trace_id));
    sentry_value_set_by_key(
        sentry_value_get_by_key(propagation_context, "trace"), "span_id",
        sentry__value_new_span_uuid(&span_id));
    sentry__generate_sample_rand(
        sentry_value_get_by_key(propagation_context, "trace"));
}

#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_PS)                 \
    || defined(SENTRY_PLATFORM_XBOX)
int
sentry__native_init(sentry_options_t *options)
#else
int
sentry_init(sentry_options_t *options)
#endif
{
    // pre-init here, so we can consistently use bailing out to :fail
    sentry_transport_t *transport = NULL;

    SENTRY__MUTEX_INIT_DYN_ONCE(g_options_lock);
    // Stop the app hang watchdog before locking options. The watchdog thread
    // calls sentry__capture_event which acquires g_options_lock; joining it
    // while holding the lock would deadlock. The sentry_close() below would
    // otherwise stop it while we still hold the lock. It's a no-op when the
    // watchdog isn't running.
    sentry__app_hang_monitor_stop();

    // this function is to be called only once, so we do not allow more than one
    // caller
    sentry__mutex_lock(&g_options_lock);

    sentry_close();

    sentry__client_report_reset();

    sentry_logger_t logger = { NULL, NULL, SENTRY_LEVEL_DEBUG };

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
    SENTRY_INFOF("using database path \"%s\"", options->database_path->path);

    // try to create and lock our run folder as early as possibly, since it is
    // fallible. since it does locking, it will not interfere with run folder
    // enumeration.
    options->run = sentry__run_new(options->database_path);
    if (!options->run) {
        SENTRY_WARN("failed to initialize run directory");
        goto fail;
    }
    options->run->require_user_consent = options->require_user_consent;

    sentry__run_load_user_consent(options->run, options->database_path);

    if (!options->dsn || !options->dsn->is_valid) {
        const char *raw_dsn = sentry_options_get_dsn(options);
        SENTRY_WARNF(
            "the provided DSN \"%s\" is not valid", raw_dsn ? raw_dsn : "");
    }

    sentry__run_load_installation_id(options->run, options->database_path,
        options->dsn ? options->dsn->public_key : NULL);

    if (transport) {
        if (sentry__transport_startup(transport, options) != 0) {
#ifdef SENTRY_PLATFORM_NX
            // A warning with more details is logged in the downstream SDK.
            // Also, we want to continue - crash capture doesn't need transport.
            sentry__transport_shutdown(transport, 0);
            sentry_options_set_transport(options, NULL);
            transport = NULL;
#else
            SENTRY_WARN("failed to initialize transport");
            goto fail;
#endif
        }
    }

    uint64_t last_crash = 0;

    // and then we will start the backend, since it requires a valid run
    sentry_backend_t *backend = options->backend;
    if (backend && backend->startup_func) {
        SENTRY_DEBUG("starting backend");
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
        generate_propagation_context(scope->propagation_context);
        scope->release = sentry__string_clone(options->release);
        scope->environment = sentry__string_clone(options->environment);
        scope->attachments = options->attachments;
        options->attachments = NULL;

        sentry__ringbuffer_set_max_size(
            scope->breadcrumbs, options->max_breadcrumbs);

        sentry__scope_update_dsc(scope, options);
    }
    if (backend && backend->user_consent_changed_func) {
        backend->user_consent_changed_func(backend);
    }

#ifdef SENTRY_INTEGRATION_QT
    SENTRY_DEBUG("setting up Qt integration");
    sentry_integration_setup_qt();
#endif

#if defined(SENTRY_PLATFORM_WINDOWS)                                           \
    && (!defined(SENTRY_BUILD_SHARED) || defined(SENTRY_PLATFORM_XBOX))
    // This function must be positioned so that any dependents on its cached
    // functions are invoked after it.
    sentry__init_cached_kernel32_functions();
#endif

    // after initializing the transport, we will submit all the unsent envelopes
    // and handle remaining sessions.
    SENTRY_DEBUG("processing and pruning old runs");
    sentry__process_old_runs(options, last_crash);
    if (backend && backend->prune_database_func) {
        backend->prune_database_func(backend);
    }

    if (options->cache_keep || options->http_retry) {
        if (!sentry__transport_submit_cleanup(options->transport, options)) {
            sentry__cleanup_cache(options);
        }
    }

    if (options->auto_session_tracking) {
        sentry_start_session();
    }

    if (options->enable_logs) {
        sentry__logs_startup(options);
    }

    if (options->enable_metrics) {
        sentry__metrics_startup(options);
    }

    if (options->enable_app_hang_tracking) {
        sentry__app_hang_monitor_start(options);
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
        // flush logs and metrics in parallel
        uintptr_t ltoken = 0;
        uintptr_t mtoken = 0;
        if (options->enable_logs) {
            ltoken = sentry__logs_force_flush_begin();
        }
        if (options->enable_metrics) {
            mtoken = sentry__metrics_force_flush_begin();
        }
        if (ltoken) {
            sentry__logs_force_flush_wait(ltoken);
        }
        if (mtoken) {
            sentry__metrics_force_flush_wait(mtoken);
        }
        rv = sentry__transport_flush(options->transport, timeout);
    }
    return rv;
}

int
sentry_close(void)
{
    // Shutdown logs and metrics before locking options to ensure they are
    // flushed. This prevents a potential deadlock on the options during
    // envelope creation.
    SENTRY_WITH_OPTIONS (options) {
        if (options->enable_logs) {
            sentry__logs_shutdown(options->shutdown_timeout);
        }
        if (options->enable_metrics) {
            sentry__metrics_shutdown(options->shutdown_timeout);
        }
    }

    // Stop it before locking options to prevent a deadlock. The watchdog
    // thread acquires the g_options_lock when it calls sentry__capture_event.
    // It's a no-op when disabled.
    sentry__app_hang_monitor_stop();

    SENTRY__MUTEX_INIT_DYN_ONCE(g_options_lock);
    // this function is to be called only once, so we do not allow more than one
    // caller
    sentry__mutex_lock(&g_options_lock);
    sentry_options_t *options = g_options;

    size_t dumped_envelopes = 0;
    if (options) {
        sentry_end_session();
        if (options->backend && options->backend->shutdown_func) {
            SENTRY_DEBUG("shutting down backend");
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
        if ((!options->backend
                || !options->backend->can_capture_after_shutdown)) {
            sentry__run_clean(options->run, false);
        }
        g_options = NULL;
        sentry_options_free(options);
    } else {
        SENTRY_WARN("sentry_close() called, but options was empty");
    }

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
        if (sentry__atomic_store(&options->run->user_consent, new_val)
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
                // flush any envelopes cached while consent was revoked
                sentry_transport_retry(options->transport);
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
        rv = (sentry_user_consent_t)(int)sentry__atomic_fetch(
            &options->run->user_consent);
    }
    return rv;
}

int
sentry_user_consent_is_required(void)
{
    int required = 0;
    SENTRY_WITH_OPTIONS (options) {
        required = options->require_user_consent;
    }
    return required;
}

void
sentry__capture_envelope(sentry_transport_t *transport,
    sentry_envelope_t *envelope, const sentry_options_t *options)
{
    if (!sentry__run_should_skip_upload(options->run)) {
        sentry__transport_send_envelope(transport, envelope);
        return;
    }
    bool cached = false;
    if (options->cache_keep || options->http_retry) {
        int retry_count = options->http_retry ? 0 : -1;
        cached = sentry__run_write_cache(options->run, envelope, retry_count);
        if (cached && !sentry__run_should_skip_upload(options->run)) {
            // consent given meanwhile -> trigger retry to avoid waiting
            // until the next retry poll
            sentry_transport_retry(options->transport);
        }
    }
    SENTRY_INFO(cached ? "caching envelope due to missing user consent"
                       : "discarding envelope due to missing user consent");
    sentry_envelope_free(envelope);
}

void
sentry_capture_envelope(sentry_envelope_t *envelope)
{
    if (!envelope) {
        return;
    }
    SENTRY_WITH_OPTIONS (options) {
        sentry__capture_envelope(options->transport, envelope, options);
    }
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
        return sentry__capture_event(event, NULL);
    }
}

sentry_uuid_t
sentry_capture_event_with_scope(sentry_value_t event, sentry_scope_t *scope)
{
    if (sentry__event_is_transaction(event)) {
        return sentry_uuid_nil();
    } else {
        return sentry__capture_event(event, scope);
    }
}

#ifndef SENTRY_UNITTEST
static
#endif
    bool
    sentry__roll_dice(double probability)
{
    uint64_t rnd;
    return probability >= 1.0 || sentry__getrandom(&rnd, sizeof(rnd))
        || ((double)rnd / (double)UINT64_MAX) <= probability;
}

sentry_uuid_t
sentry__capture_event(sentry_value_t event, sentry_scope_t *local_scope)
{
    // `event_id` is only used as an argument to pure output parameters.
    // Initialization only happens to prevent compiler warnings.
    sentry_uuid_t event_id = sentry_uuid_nil();
    sentry_envelope_t *envelope = NULL;

    bool was_captured = false;
    bool was_sent = false;
    SENTRY_WITH_OPTIONS (options) {
        was_captured = true;

        if (sentry__event_is_transaction(event)) {
            envelope = sentry__prepare_transaction(options, event, &event_id);
        } else {
            envelope = sentry__prepare_event(
                options, event, &event_id, true, local_scope);
        }
        if (envelope) {
            // Accept a racy read here, since SENTRY_WITH_OPTIONS only prevents
            // the options from being deallocated while we use them, but no lock
            // is active and the session could change during session shutdown.
            // We recheck below inside the lock and don't pay for a lock here.
            // This also means we accept a missed window of opportunity if an
            // event is being sent concurrently to session initialization, which
            // is an acceptable design trade-off.
            SENTRY_TSAN_IGNORE_READS_BEGIN();
            bool has_session = options->session;
            SENTRY_TSAN_IGNORE_READS_END();
            if (has_session) {
                sentry_options_t *mut_options = sentry__options_lock();
                // recheck inside the lock since our previous read is racy.
                if (mut_options->session) {
                    sentry__envelope_add_session(
                        envelope, mut_options->session);
                    // we're assuming that if a session is added to an envelope,
                    // it will be sent onwards.  This means we now need to set
                    // the init flag to false because we're no longer in the
                    // initial session update.
                    mut_options->session->init = false;
                }
                sentry__options_unlock();
            }

            bool should_skip = !sentry__roll_dice(options->sample_rate);
            if (should_skip) {
                SENTRY_INFO("throwing away event due to sample rate");
                sentry__client_report_discard(SENTRY_DISCARD_REASON_SAMPLE_RATE,
                    SENTRY_DATA_CATEGORY_ERROR, 1);
                sentry_envelope_free(envelope);
            } else {
                sentry__capture_envelope(options->transport, envelope, options);
                was_sent = true;
            }
        }
    }
    if (!was_captured) {
        sentry_value_decref(event);
    }
    return was_sent ? event_id : sentry_uuid_nil();
}

#ifndef SENTRY_UNITTEST
static
#endif
    bool
    sentry__should_send_transaction(
        sentry_value_t tx_ctx, sentry_sampling_context_t *sampling_ctx)
{
    sentry_value_t context_setting = sentry_value_get_by_key(tx_ctx, "sampled");
    bool sampled = sentry_value_is_null(context_setting)
        ? false
        : sentry_value_is_true(context_setting);
    sampling_ctx->parent_sampled
        = sentry_value_is_null(context_setting) ? NULL : &sampled;

    const int parent_sampled_int = sampling_ctx->parent_sampled
        ? (int)*sampling_ctx->parent_sampled
        : -1; // -1 signifies no parent sampling decision
    bool send = false;
    SENTRY_WITH_OPTIONS (options) {
        if (options->traces_sampler) {
            const double result
                = ((sentry_traces_sampler_function)options->traces_sampler)(
                    sampling_ctx->transaction_context,
                    sampling_ctx->custom_sampling_context,
                    sampling_ctx->parent_sampled == NULL ? NULL
                                                         : &parent_sampled_int,
                    options->traces_sampler_data);
            send = sampling_ctx->sample_rand < result;
        } else {
            if (sampling_ctx->parent_sampled != NULL) {
                send = *sampling_ctx->parent_sampled;
            } else {
                send = sampling_ctx->sample_rand < options->traces_sample_rate;
            }
        }
    }
    if (sampling_ctx->parent_sampled != NULL) {
        sampling_ctx->parent_sampled = NULL;
    }
    return send;
}

sentry_envelope_t *
sentry__prepare_event(const sentry_options_t *options, sentry_value_t event,
    sentry_uuid_t *event_id, bool invoke_before_send,
    sentry_scope_t *local_scope)
{
    sentry_envelope_t *envelope = NULL;

    if (event_is_considered_error(event)) {
        sentry__record_errors_on_current_session(1);
    }

    sentry_attachment_t *all_attachments = NULL;
    if (local_scope) {
        SENTRY_DEBUG("merging local scope into event");
        sentry_scope_mode_t mode = SENTRY_SCOPE_BREADCRUMBS;
        sentry__scope_apply_to_event(local_scope, options, event, mode);
        sentry__attachments_extend(&all_attachments, local_scope->attachments);
        sentry__scope_free(local_scope);
    }

    SENTRY_WITH_SCOPE (scope) {
        SENTRY_DEBUG("merging global scope into event");
        sentry_scope_mode_t mode = SENTRY_SCOPE_ALL;
        if (!options->symbolize_stacktraces) {
            mode &= ~SENTRY_SCOPE_STACKTRACES;
        }
        if (all_attachments) {
            sentry__attachments_extend(&all_attachments, scope->attachments);
        }
        sentry__scope_apply_to_event(scope, options, event, mode);
    }

    if (options->before_send_func && invoke_before_send) {
        SENTRY_DEBUG("invoking `before_send` hook");
        event
            = options->before_send_func(event, NULL, options->before_send_data);
        if (sentry_value_is_null(event)) {
            SENTRY_DEBUG("event was discarded by the `before_send` hook");
            sentry__client_report_discard(SENTRY_DISCARD_REASON_BEFORE_SEND,
                SENTRY_DATA_CATEGORY_ERROR, 1);
            return NULL;
        }
    }

    sentry__ensure_event_id(event, event_id);
    envelope = sentry__envelope_new();
    if (!envelope || !sentry__envelope_add_event(envelope, event)) {
        goto fail;
    }

    SENTRY_WITH_SCOPE (scope) {
        if (all_attachments) {
            // all attachments merged from multiple scopes
            sentry__envelope_add_attachments(
                envelope, all_attachments, options);
        } else {
            // only global scope has attachments
            sentry__envelope_add_attachments(
                envelope, scope->attachments, options);
        }
        if (options->run) {
            sentry__cache_attachment_refs(envelope,
                all_attachments ? all_attachments : scope->attachments, options,
                options->run->cache_path, options->run->run_path);
        }
    }

    sentry__attachments_free(all_attachments);

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
        SENTRY_DEBUG("merging scope into transaction");
        // Don't include debugging info
        sentry_scope_mode_t mode = SENTRY_SCOPE_ALL & ~SENTRY_SCOPE_MODULES
            & ~SENTRY_SCOPE_STACKTRACES;
        sentry__scope_apply_to_event(scope, options, transaction, mode);
    }

    if (options->before_transaction_func) {
        SENTRY_DEBUG("invoking `before_transaction` hook");
        transaction = options->before_transaction_func(
            transaction, options->before_transaction_data);
        if (sentry_value_is_null(transaction)) {
            SENTRY_DEBUG(
                "transaction was discarded by the `before_transaction` hook");
            sentry__client_report_discard(SENTRY_DISCARD_REASON_BEFORE_SEND,
                SENTRY_DATA_CATEGORY_TRANSACTION, 1);
            return NULL;
        }
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

static sentry_envelope_t *
prepare_user_report(sentry_value_t user_report)
{
    sentry_envelope_t *envelope = NULL;

    envelope = sentry__envelope_new();
    if (!envelope || !sentry__envelope_add_user_report(envelope, user_report)) {
        goto fail;
    }

    return envelope;

fail:
    SENTRY_WARN("dropping user report");
    sentry_envelope_free(envelope);
    return NULL;
}

static sentry_envelope_t *
prepare_user_feedback(const sentry_options_t *options,
    sentry_value_t user_feedback, sentry_hint_t *hint)
{
    sentry_envelope_t *envelope = NULL;

    envelope = sentry__envelope_new();
    if (!envelope
        || !sentry__envelope_add_user_feedback(envelope, user_feedback)) {
        goto fail;
    }

    if (hint && hint->attachments) {
        sentry__envelope_add_attachments(envelope, hint->attachments, options);
        if (options->run) {
            sentry__cache_attachment_refs(envelope, hint->attachments, options,
                options->run->cache_path, options->run->run_path);
        }
    }

    return envelope;

fail:
    SENTRY_WARN("dropping user feedback");
    sentry_envelope_free(envelope);
    sentry_value_decref(user_feedback);
    return NULL;
}

void
sentry_handle_exception(const sentry_ucontext_t *uctx)
{
    // Disarm the app-hang monitor to avoid duplicate capture.
    sentry__app_hang_set_active(false);

    SENTRY_WITH_OPTIONS (options) {
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
            sentry__session_sync_user(options->session, user,
                options->run ? options->run->installation_id : NULL);
            sentry__run_write_session(options->run, options->session);
        }
        sentry__options_unlock();
    }

    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_scope_set_user(scope, user);
    }
}

void
sentry_remove_user(void)
{
    sentry_set_user(sentry_value_new_null());
}

void
sentry_set_release_n(const char *release, size_t release_len)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_free(scope->release);
        scope->release = sentry__string_clone_n(release, release_len);
        sentry_value_set_by_key(scope->dynamic_sampling_context, "release",
            sentry_value_new_string(scope->release));
    }
}

void
sentry_set_release(const char *release)
{
    sentry_set_release_n(release, sentry__guarded_strlen(release));
}

void
sentry_set_environment_n(const char *environment, size_t environment_len)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_free(scope->environment);
        scope->environment
            = sentry__string_clone_n(environment, environment_len);
        sentry_value_set_by_key(scope->dynamic_sampling_context, "environment",
            sentry_value_new_string(scope->environment));
    }
}

void
sentry_set_environment(const char *environment)
{
    sentry_set_environment_n(environment, sentry__guarded_strlen(environment));
}

void
sentry_add_breadcrumb(sentry_value_t breadcrumb)
{
    bool discarded = false;
    SENTRY_WITH_OPTIONS (options) {
        if (options->before_breadcrumb_func) {
            SENTRY_DEBUG("invoking `before_breadcrumb` hook");
            breadcrumb = options->before_breadcrumb_func(
                breadcrumb, options->before_breadcrumb_data);
            if (sentry_value_is_null(breadcrumb)) {
                SENTRY_DEBUG("breadcrumb was discarded by the "
                             "`before_breadcrumb` hook");
                discarded = true;
            }
        }

        if (!discarded && options->backend
            && options->backend->add_breadcrumb_func) {
            // the hook will *not* take ownership
            options->backend->add_breadcrumb_func(
                options->backend, breadcrumb, options);
        }
    }

    if (discarded) {
        return;
    }

    // the `no_flush` will avoid triggering *both* scope-change and
    // breadcrumb-add events.
    SENTRY_WITH_SCOPE_MUT_NO_FLUSH (scope) {
        sentry_scope_add_breadcrumb(scope, breadcrumb);
    }
}

void
sentry_set_tag(const char *key, const char *value)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_scope_set_tag(scope, key, value);
    }
}

void
sentry_set_tag_n(
    const char *key, size_t key_len, const char *value, size_t value_len)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_scope_set_tag_n(scope, key, key_len, value, value_len);
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
        sentry_scope_set_extra(scope, key, value);
    }
}

void
sentry_set_extra_n(const char *key, size_t key_len, sentry_value_t value)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_scope_set_extra_n(scope, key, key_len, value);
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
sentry_set_attribute(const char *key, sentry_value_t attribute)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry__scope_set_attribute(scope, key, attribute);
    }
}

void
sentry_set_attribute_n(
    const char *key, size_t key_len, sentry_value_t attribute)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry__scope_set_attribute_n(scope, key, key_len, attribute);
    }
}

void
sentry_remove_attribute(const char *key)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry__scope_remove_attribute(scope, key);
    }
}

void
sentry_remove_attribute_n(const char *key, size_t key_len)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry__scope_remove_attribute_n(scope, key, key_len);
    }
}

void
sentry_set_context(const char *key, sentry_value_t value)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_scope_set_context(scope, key, value);
    }
}

void
sentry_set_context_n(const char *key, size_t key_len, sentry_value_t value)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_scope_set_context_n(scope, key, key_len, value);
    }
}

void
sentry_update_context(const char *key, sentry_value_t value)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_scope_update_context(scope, key, value);
    }
}

void
sentry_update_context_n(const char *key, size_t key_len, sentry_value_t value)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_scope_update_context_n(scope, key, key_len, value);
    }
}

void
sentry__set_propagation_context(const char *key, sentry_value_t value)
{
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry_value_set_by_key(scope->propagation_context, key, value);
    }
}

void
sentry__apply_attributes(sentry_value_t telemetry, sentry_value_t attributes)
{
    SENTRY_WITH_SCOPE (scope) {
        sentry__scope_apply_attributes(scope, telemetry, attributes);
        if (scope->environment) {
            sentry__value_add_attribute(attributes,
                sentry_value_new_string(scope->environment), "string",
                "sentry.environment");
        }
        if (scope->release) {
            sentry__value_add_attribute(attributes,
                sentry_value_new_string(scope->release), "string",
                "sentry.release");
        }
    }
    SENTRY_WITH_OPTIONS (options) {
        sentry__value_add_attribute(attributes,
            sentry_value_new_string(sentry_options_get_sdk_name(options)),
            "string", "sentry.sdk.name");
    }
    sentry__value_add_attribute(attributes,
        sentry_value_new_string(sentry_sdk_version()), "string",
        "sentry.sdk.version");
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
    va_list va;
    va_start(va, fingerprint_len);

    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry__scope_set_fingerprint_nva(
            scope, fingerprint, fingerprint_len, va);
    }

    va_end(va);
}

void
sentry_set_fingerprint(const char *fingerprint, ...)
{
    va_list va;
    va_start(va, fingerprint);

    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry__scope_set_fingerprint_va(scope, fingerprint, va);
    }

    va_end(va);
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
sentry_set_trace(const char *trace_id, const char *parent_span_id)
{
    sentry_set_trace_n(trace_id, sentry__guarded_strlen(trace_id),
        parent_span_id, sentry__guarded_strlen(parent_span_id));
}

void
sentry_set_trace_n(const char *trace_id, size_t trace_id_len,
    const char *parent_span_id, size_t parent_span_id_len)
{
    sentry_value_t context = sentry_value_new_null();

    SENTRY_WITH_SCOPE_MUT (scope) {
        context = sentry_value_new_object();

        sentry_value_set_by_key(
            context, "type", sentry_value_new_string("trace"));

        sentry_value_set_by_key(context, "trace_id",
            sentry_value_new_string_n(trace_id, trace_id_len));
        sentry_value_set_by_key(context, "parent_span_id",
            sentry_value_new_string_n(parent_span_id, parent_span_id_len));

        sentry_uuid_t span_id = sentry_uuid_new_v4();
        sentry_value_set_by_key(
            context, "span_id", sentry__value_new_span_uuid(&span_id));
        scope->trace_managed = false;
    }

    if (!sentry_value_is_null(context)) {
        sentry__generate_sample_rand(context);

        sentry__set_propagation_context("trace", context);

        SENTRY_WITH_OPTIONS (options) {
            SENTRY_WITH_SCOPE_MUT (scope) {
                sentry__scope_update_dsc(scope, options);
            }
        }
    }
}

void
sentry_regenerate_trace(void)
{
    SENTRY_WITH_OPTIONS (options) {
        SENTRY_WITH_SCOPE_MUT (scope) {
            generate_propagation_context(scope->propagation_context);
            scope->trace_managed = false;
            sentry__scope_update_dsc(scope, options);
        }
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
        sentry_scope_set_level(scope, level);
    }
}

sentry_transaction_t *
sentry_transaction_start(sentry_transaction_context_t *opaque_tx_ctx,
    sentry_value_t custom_sampling_ctx)
{
    return sentry_transaction_start_ts(
        opaque_tx_ctx, custom_sampling_ctx, sentry__usec_time());
}

sentry_transaction_t *
sentry_transaction_start_ts(sentry_transaction_context_t *opaque_tx_ctx,
    sentry_value_t custom_sampling_ctx, uint64_t timestamp)
{
    if (!opaque_tx_ctx) {
        return NULL;
    }

    sentry_value_t tx_ctx = opaque_tx_ctx->inner;

    // If the parent span ID is some empty-ish value, just remove it
    sentry_value_t parent_span
        = sentry_value_get_by_key(tx_ctx, "parent_span_id");
    if (sentry_value_get_length(parent_span) < 1) {
        sentry_value_remove_by_key(tx_ctx, "parent_span_id");
    }

    // The ending timestamp is stripped to avoid misleading ourselves later
    // down the line, as it is the only way to determine whether a transaction
    // has ended or not.
    sentry_value_t tx = sentry_value_new_event();
    sentry_value_remove_by_key(tx, "timestamp");

    sentry__value_merge_objects(tx, tx_ctx);

    sentry_value_t incoming = sentry_value_get_by_key(tx, "incoming_dsc");
    if (!sentry_value_is_null(incoming)) {
        SENTRY_WITH_OPTIONS (options) {
            SENTRY_WITH_SCOPE_MUT (scope) {
                if (sentry__trace_can_continue(incoming, options)) {
                    // Freeze only when the upstream actually sent DSC values;
                    // a sentry-trace-only signal leaves incoming empty, in
                    // which case the SDK builds its own DSC.
                    if (sentry_value_get_length(incoming) > 0) {
                        sentry__scope_freeze_dsc(scope, incoming);
                    } else {
                        sentry__scope_update_dsc(scope, options);
                    }
                } else {
                    // Fork: ignore upstream trace, become head of a new trace.
                    // Regenerate the scope's propagation context so events
                    // captured outside this transaction also carry the new
                    // trace_id, and align the tx's trace_id with it.
                    generate_propagation_context(scope->propagation_context);
                    sentry_value_t scope_trace_id = sentry_value_get_by_key(
                        sentry_value_get_by_key(
                            scope->propagation_context, "trace"),
                        "trace_id");
                    sentry_value_incref(scope_trace_id);
                    sentry_value_set_by_key(tx, "trace_id", scope_trace_id);
                    sentry_value_remove_by_key(tx, "parent_span_id");
                    sentry_value_remove_by_key(tx, "sampled");
                    sentry__scope_update_dsc(scope, options);
                }
            }
        }
    }
    sentry_value_remove_by_key(tx, "incoming_dsc");

    double sample_rand = 1.0;
    SENTRY_WITH_SCOPE (scope) {
        sample_rand = sentry_value_as_double(sentry_value_get_by_key(
            sentry_value_get_by_key(scope->propagation_context, "trace"),
            "sample_rand"));
    }
    sentry_sampling_context_t sampling_ctx
        = { opaque_tx_ctx, custom_sampling_ctx, NULL, sample_rand };

    bool should_sample = sentry__should_send_transaction(tx, &sampling_ctx);
    sentry_value_set_by_key(
        tx, "sampled", sentry_value_new_bool(should_sample));
    sentry_value_decref(custom_sampling_ctx);

    sentry_value_set_by_key(tx, "start_timestamp",
        sentry__value_new_string_owned(
            sentry__usec_time_to_iso8601(timestamp)));

    sentry__transaction_context_free(opaque_tx_ctx);
    return sentry__transaction_new(tx);
}

sentry_uuid_t
sentry_transaction_finish(sentry_transaction_t *opaque_tx)
{
    return sentry_transaction_finish_ts(opaque_tx, sentry__usec_time());
}

sentry_uuid_t
sentry_transaction_finish_ts(
    sentry_transaction_t *opaque_tx, uint64_t timestamp)
{
    sentry_value_t tx = sentry__transaction_finish_value(opaque_tx, timestamp);
    if (sentry_value_is_null(tx)) {
        return sentry_uuid_nil();
    }

    // This takes ownership of the transaction, generates an event ID, merges
    // scope
    return sentry__capture_event(tx, NULL);
}

sentry_value_t
sentry__transaction_finish_value(
    sentry_transaction_t *opaque_tx, uint64_t timestamp)
{
    if (!opaque_tx || sentry_value_is_null(opaque_tx->inner)) {
        SENTRY_WARN("no transaction available to finish");
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
        // if the SDK manages the trace (rather than the user or a downstream
        // SDK) we break propagation context traces at transaction boundaries.
        if (scope->trace_managed) {
            sentry_value_t txn_trace_id
                = sentry_value_get_by_key(tx, "trace_id");
            sentry_value_incref(txn_trace_id);

            sentry_value_set_by_key(
                sentry_value_get_by_key(scope->propagation_context, "trace"),
                "trace_id", txn_trace_id);
        }
    }
    // The sampling decision should already be made for transactions
    // during their construction. No need to recalculate here. See
    // `sentry__should_skip_transaction`.
    sentry_value_t sampled = sentry_value_get_by_key(tx, "sampled");
    if (!sentry_value_is_true(sampled)) {
        SENTRY_INFO("throwing away transaction due to sample rate or "
                    "user-provided sampling value in transaction context");
        sentry_value_decref(tx);
        goto fail;
    }
    sentry_value_remove_by_key(tx, "sampled");

    sentry_value_set_by_key(tx, "type", sentry_value_new_string("transaction"));
    sentry_value_set_by_key(tx, "timestamp",
        sentry__value_new_string_owned(
            sentry__usec_time_to_iso8601(timestamp)));
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
    sentry_value_set_by_key(
        trace_context, "data", sentry_value_get_by_key(tx, "data"));
    sentry_value_incref(sentry_value_get_by_key(tx, "data"));
    sentry_value_set_by_key(contexts, "trace", trace_context);
    sentry_value_set_by_key(tx, "contexts", contexts);

    // clean up trace context fields
    sentry_value_remove_by_key(tx, "trace_id");
    sentry_value_remove_by_key(tx, "span_id");
    sentry_value_remove_by_key(tx, "parent_span_id");
    sentry_value_remove_by_key(tx, "op");
    sentry_value_remove_by_key(tx, "description");
    sentry_value_remove_by_key(tx, "status");
    sentry_value_remove_by_key(tx, "data");

    sentry__transaction_decref(opaque_tx);

    return tx;
fail:
    sentry__transaction_decref(opaque_tx);
    return sentry_value_new_null();
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
    return sentry_transaction_start_child_ts_n(opaque_parent, operation,
        operation_len, description, description_len, sentry__usec_time());
}

sentry_span_t *
sentry_transaction_start_child(sentry_transaction_t *opaque_parent,
    const char *operation, const char *description)
{
    return sentry_transaction_start_child_n(opaque_parent, operation,
        sentry__guarded_strlen(operation), description,
        sentry__guarded_strlen(description));
}

sentry_span_t *
sentry_transaction_start_child_ts_n(sentry_transaction_t *opaque_parent,
    const char *operation, size_t operation_len, const char *description,
    size_t description_len, const uint64_t timestamp)
{
    if (!opaque_parent || sentry_value_is_null(opaque_parent->inner)) {
        SENTRY_WARN("no transaction available to create a child under");
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
        (sentry_slice_t) { description, description_len }, timestamp);
    return sentry__span_new(opaque_parent, span);
}

sentry_span_t *
sentry_transaction_start_child_ts(sentry_transaction_t *opaque_parent,
    const char *operation, const char *description, const uint64_t timestamp)
{
    return sentry_transaction_start_child_ts_n(opaque_parent, operation,
        sentry__guarded_strlen(operation), description,
        sentry__guarded_strlen(description), timestamp);
}

sentry_span_t *
sentry_span_start_child_n(sentry_span_t *opaque_parent, const char *operation,
    size_t operation_len, const char *description, size_t description_len)
{
    return sentry_span_start_child_ts_n(opaque_parent, operation, operation_len,
        description, description_len, sentry__usec_time());
}

sentry_span_t *
sentry_span_start_child(sentry_span_t *opaque_parent, const char *operation,
    const char *description)
{
    return sentry_span_start_child_n(opaque_parent, operation,
        sentry__guarded_strlen(operation), description,
        sentry__guarded_strlen(description));
}

sentry_span_t *
sentry_span_start_child_ts_n(sentry_span_t *opaque_parent,
    const char *operation, size_t operation_len, const char *description,
    size_t description_len, uint64_t timestamp)
{
    if (!opaque_parent || sentry_value_is_null(opaque_parent->inner)) {
        SENTRY_WARN("no parent span available to create a child span under");
        return NULL;
    }
    if (!opaque_parent->transaction) {
        SENTRY_WARN("no root transaction to create a child span under");
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
        (sentry_slice_t) { description, description_len }, timestamp);

    return sentry__span_new(opaque_parent->transaction, span);
}

sentry_span_t *
sentry_span_start_child_ts(sentry_span_t *opaque_parent, const char *operation,
    const char *description, uint64_t timestamp)
{
    return sentry_span_start_child_ts_n(opaque_parent, operation,
        sentry__guarded_strlen(operation), description,
        sentry__guarded_strlen(description), timestamp);
}

void
sentry_span_finish(sentry_span_t *opaque_span)
{
    sentry_span_finish_ts(opaque_span, sentry__usec_time());
}

void
sentry_span_finish_ts(sentry_span_t *opaque_span, uint64_t timestamp)
{
    if (!opaque_span || sentry_value_is_null(opaque_span->inner)) {
        SENTRY_WARN("no span to finish");
        goto fail;
    }

    sentry_transaction_t *opaque_root_transaction = opaque_span->transaction;
    if (!opaque_root_transaction
        || sentry_value_is_null(opaque_root_transaction->inner)) {
        SENTRY_WARN(
            "no root transaction to finish span on, aborting span finish");
        goto fail;
    }

    sentry__transaction_remove_child(opaque_root_transaction, opaque_span);

    sentry_value_t root_transaction = opaque_root_transaction->inner;

    if (!sentry_value_is_true(
            sentry_value_get_by_key(root_transaction, "sampled"))) {
        SENTRY_INFO("root transaction is unsampled, dropping span");
        goto fail;
    }

    if (!sentry_value_is_null(
            sentry_value_get_by_key(root_transaction, "timestamp"))) {
        SENTRY_WARN("span's root transaction is already finished, aborting "
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
        SENTRY_INFO("span is unsampled, dropping span");
        sentry_value_decref(span);
        goto fail;
    }

    if (!sentry_value_is_null(sentry_value_get_by_key(span, "timestamp"))) {
        SENTRY_WARN("span is already finished, aborting span finish");
        sentry_value_decref(span);
        goto fail;
    }

    sentry_value_set_by_key(span, "timestamp",
        sentry__value_new_string_owned(
            sentry__usec_time_to_iso8601(timestamp)));
    sentry_value_remove_by_key(span, "sampled");

    size_t max_spans = SENTRY_SPANS_MAX;
    SENTRY_WITH_OPTIONS (options) {
        max_spans = options->max_spans;
    }

    sentry_value_t spans = sentry_value_get_by_key(root_transaction, "spans");

    if (sentry_value_get_length(spans) >= max_spans) {
        SENTRY_WARN("reached maximum number of spans for transaction, "
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

void
sentry_capture_user_feedback(sentry_value_t user_report)
{
    sentry_envelope_t *envelope = NULL;

    SENTRY_WITH_OPTIONS (options) {
        envelope = prepare_user_report(user_report);
        if (envelope) {
            sentry__capture_envelope(options->transport, envelope, options);
        }
    }
    sentry_value_decref(user_report);
}

void
sentry_capture_feedback(sentry_value_t user_feedback)
{
    // Reuse the implementation with NULL hint
    sentry_capture_feedback_with_hint(user_feedback, NULL);
}

void
sentry_capture_feedback_with_hint(
    sentry_value_t user_feedback, sentry_hint_t *hint)
{
    sentry_envelope_t *envelope = NULL;

    SENTRY_WITH_OPTIONS (options) {
        envelope = prepare_user_feedback(options, user_feedback, hint);
        if (envelope) {
            sentry__capture_envelope(options->transport, envelope, options);
        }
    }

    if (hint) {
        sentry__hint_free(hint);
    }
}

bool
sentry__launch_external_crash_reporter(
    const sentry_options_t *options, sentry_envelope_t *envelope)
{
    if (!options || !options->run || !options->external_crash_reporter
        || sentry__string_empty(options->external_crash_reporter->path)) {
        return false;
    }

    if (options->cache_keep == SENTRY_CACHE_KEEP_ALWAYS) {
        sentry__run_write_cache(options->run, envelope, -1);
    }

    sentry_uuid_t event_id = sentry__envelope_get_event_id(envelope);
    char *envelope_filename = sentry__uuid_as_filename(&event_id, ".envelope");
    if (!envelope_filename) {
        return false;
    }

    sentry_path_t *report_path
        = sentry__path_join_str(options->run->external_path, envelope_filename);
    if (!report_path) {
        sentry_free(envelope_filename);
        return false;
    }

    // capture the envelope with the disk transport
    sentry_transport_t *disk_transport
        = sentry_new_external_disk_transport(options->run);
    if (!disk_transport) {
        sentry__path_free(report_path);
        sentry_free(envelope_filename);
        return false;
    }
    if (options->cache_keep) {
        if (!sentry__envelope_is_raw(envelope)) {
            sentry__envelope_set_header(envelope, "cache_dir",
                sentry_value_new_string(options->run->cache_path->path));
        } else {
            SENTRY_WARN("failed to add cache_dir to external crash report");
        }
    }
    sentry__transport_send_envelope(disk_transport, envelope);
    sentry__transport_dump_queue(disk_transport, options->run);
    sentry_transport_free(disk_transport);

    sentry__process_spawn(
        options->external_crash_reporter, report_path->path, NULL);
    sentry__path_free(report_path);
    sentry_free(envelope_filename);
    return true;
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

sentry_uuid_t
sentry_capture_minidump(const char *path)
{
    return sentry_capture_minidump_n(path, sentry__guarded_strlen(path));
}

static sentry_uuid_t
capture_minidump(sentry_path_t *dump_path)
{
    if (!dump_path) {
        SENTRY_WARN(
            "sentry_capture_minidump() failed due to null path to minidump");
        return sentry_uuid_nil();
    }

    SENTRY_DEBUGF("Capturing minidump \"%s\"", dump_path->path);

    SENTRY_WITH_OPTIONS (options) {
        sentry_uuid_t event_id;
        sentry_value_t event = sentry_value_new_event();
        sentry_value_set_by_key(
            event, "level", sentry__value_new_level(SENTRY_LEVEL_FATAL));
        sentry_envelope_t *envelope
            = sentry__prepare_event(options, event, &event_id, true, NULL);

        if (!envelope || sentry_uuid_is_nil(&event_id)) {
            sentry_envelope_free(envelope);
        } else {
            if (options->enable_large_attachments && options->run
                && sentry__path_get_size(dump_path)
                    >= SENTRY_LARGE_ATTACHMENT_SIZE) {
                sentry_attachment_t tmp = { 0 };
                tmp.path = dump_path;
                tmp.type = (char *)SENTRY_ATTACHMENT_TYPE_MINIDUMP;
                if (!sentry__cache_attachment_ref(
                        envelope, &tmp, options->run->cache_path, NULL)) {
                    SENTRY_WARN("failed to cache minidump attachment-ref");
                    sentry__client_report_discard(
                        SENTRY_DISCARD_REASON_SEND_ERROR,
                        SENTRY_DATA_CATEGORY_ATTACHMENT, 1);
                    sentry_envelope_free(envelope);
                    sentry__path_free(dump_path);
                    sentry_options_free((sentry_options_t *)options);
                    return sentry_uuid_nil();
                }
                sentry__capture_envelope(options->transport, envelope, options);
                SENTRY_INFOF(
                    "Minidump has been captured: \"%s\"", dump_path->path);
                sentry__path_free(dump_path);
                sentry_options_free((sentry_options_t *)options);
                return event_id;
            }

            // the minidump is added as an attachment, with the type
            // `event.minidump`
            sentry_envelope_item_t *item = sentry__envelope_add_from_path(
                envelope, dump_path, "attachment");

            if (!item) {
                sentry_envelope_free(envelope);
            } else {
                sentry__envelope_item_set_header(item, "attachment_type",
                    sentry_value_new_string(SENTRY_ATTACHMENT_TYPE_MINIDUMP));

                sentry__envelope_item_set_header(item, "filename",
                    sentry_value_new_string(sentry__path_filename(dump_path)));

                sentry__capture_envelope(options->transport, envelope, options);

                SENTRY_INFOF(
                    "Minidump has been captured: \"%s\"", dump_path->path);
                sentry__path_free(dump_path);

                sentry_options_free((sentry_options_t *)options);
                return event_id;
            }
        }
    }

    SENTRY_WARNF("Minidump was not captured: \"%s\"", dump_path->path);
    sentry__path_free(dump_path);

    return sentry_uuid_nil();
}

sentry_uuid_t
sentry_capture_minidump_n(const char *path, size_t path_len)
{
    sentry_path_t *dump_path = sentry__path_from_str_n(path, path_len);
    return capture_minidump(dump_path);
}

static sentry_attachment_t *
add_attachment(sentry_attachment_t *attachment)
{
    SENTRY_WITH_OPTIONS (options) {
        if (options->backend && options->backend->add_attachment_func) {
            options->backend->add_attachment_func(options->backend, attachment);
        }
    }
    SENTRY_WITH_SCOPE_MUT (scope) {
        attachment = sentry__attachments_add(&scope->attachments, attachment);
    }
    return attachment;
}

sentry_attachment_t *
sentry_attach_file(const char *path)
{
    return sentry_attach_file_n(path, sentry__guarded_strlen(path));
}

sentry_attachment_t *
sentry_attach_file_n(const char *path, size_t path_len)
{
    return add_attachment(
        sentry__attachment_from_path(sentry__path_from_str_n(path, path_len)));
}

sentry_attachment_t *
sentry_attach_bytes(const char *buf, size_t buf_len, const char *filename)
{
    return sentry_attach_bytes_n(
        buf, buf_len, filename, sentry__guarded_strlen(filename));
}

sentry_attachment_t *
sentry_attach_bytes_n(
    const char *buf, size_t buf_len, const char *filename, size_t filename_len)
{
    return add_attachment(sentry__attachment_from_buffer(
        buf, buf_len, sentry__path_from_str_n(filename, filename_len)));
}

void
sentry_clear_attachments(void)
{
    SENTRY_WITH_OPTIONS (options) {
        SENTRY_WITH_SCOPE_MUT (scope) {
            if (options->backend && options->backend->remove_attachment_func) {
                for (sentry_attachment_t *it = scope->attachments; it;
                    it = it->next) {
                    options->backend->remove_attachment_func(
                        options->backend, it);
                }
            }
            sentry__attachments_free(scope->attachments);
            scope->attachments = NULL;
        }
    }
}

void
sentry_remove_attachment(sentry_attachment_t *attachment)
{
    SENTRY_WITH_OPTIONS (options) {
        if (options->backend && options->backend->remove_attachment_func) {
            options->backend->remove_attachment_func(
                options->backend, attachment);
        }
    }
    SENTRY_WITH_SCOPE_MUT (scope) {
        sentry__attachments_remove(&scope->attachments, attachment);
    }
}

#ifdef SENTRY_PLATFORM_WINDOWS
sentry_attachment_t *
sentry_attach_filew(const wchar_t *path)
{
    size_t path_len = path ? wcslen(path) : 0;
    return sentry_attach_filew_n(path, path_len);
}

sentry_attachment_t *
sentry_attach_filew_n(const wchar_t *path, size_t path_len)
{
    return add_attachment(
        sentry__attachment_from_path(sentry__path_from_wstr_n(path, path_len)));
}

sentry_attachment_t *
sentry_attach_bytesw(const char *buf, size_t buf_len, const wchar_t *filename)
{
    size_t filename_len = filename ? wcslen(filename) : 0;
    return sentry_attach_bytesw_n(buf, buf_len, filename, filename_len);
}

sentry_attachment_t *
sentry_attach_bytesw_n(const char *buf, size_t buf_len, const wchar_t *filename,
    size_t filename_len)
{
    return add_attachment(sentry__attachment_from_buffer(
        buf, buf_len, sentry__path_from_wstr_n(filename, filename_len)));
}

sentry_uuid_t
sentry_capture_minidumpw(const wchar_t *path)
{
    size_t path_len = path ? wcslen(path) : 0;
    return sentry_capture_minidumpw_n(path, path_len);
}

sentry_uuid_t
sentry_capture_minidumpw_n(const wchar_t *path, size_t path_len)
{
    sentry_path_t *dump_path = sentry__path_from_wstr_n(path, path_len);
    return capture_minidump(dump_path);
}
#endif
