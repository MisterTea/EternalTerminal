#ifndef SENTRY_OPTIONS_H_INCLUDED
#define SENTRY_OPTIONS_H_INCLUDED

#include "sentry_boot.h"

#include "sentry_attachment.h"
#include "sentry_database.h"
#include "sentry_logger.h"
#include "sentry_session.h"
#include "sentry_utils.h"

// Defaults to 2s as per
// https://docs.sentry.io/error-reporting/configuration/?platform=native#shutdown-timeout
#define SENTRY_DEFAULT_SHUTDOWN_TIMEOUT 2000

struct sentry_backend_s;

/**
 * This is the main options struct, which is being accessed throughout all of
 * the sentry internals.
 */
struct sentry_options_s {
    double sample_rate;
    sentry_dsn_t *dsn;
    char *release;
    char *environment;
    char *dist;
    char *proxy;
    char *ca_certs;
    char *transport_thread_name;
    char *sdk_name;
    char *user_agent;
    sentry_path_t *database_path;
    sentry_path_t *handler_path;
    sentry_logger_t logger;
    size_t max_breadcrumbs;
    bool debug;
    bool auto_session_tracking;
    bool require_user_consent;
    bool symbolize_stacktraces;
    bool system_crash_reporter_enabled;
    bool attach_screenshot;
    bool crashpad_wait_for_upload;

    sentry_attachment_t *attachments;
    sentry_run_t *run;

    sentry_transport_t *transport;
    sentry_event_function_t before_send_func;
    void *before_send_data;
    sentry_crash_function_t on_crash_func;
    void *on_crash_data;
    sentry_transaction_function_t before_transaction_func;
    void *before_transaction_data;

    /* Experimentally exposed */
    double traces_sample_rate;
    sentry_traces_sampler_function traces_sampler;
    size_t max_spans;

    /* everything from here on down are options which are stored here but
       not exposed through the options API */
    struct sentry_backend_s *backend;
    sentry_session_t *session;

    long user_consent;
    long refcount;
    uint64_t shutdown_timeout;
    sentry_handler_strategy_t handler_strategy;

#ifdef SENTRY_PLATFORM_NX
    void (*network_connect_func)(void);
    bool send_default_pii;
#endif
};

/**
 * Increments the reference count and returns the options.
 */
sentry_options_t *sentry__options_incref(sentry_options_t *options);

#endif
