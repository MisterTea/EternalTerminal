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
#define SENTRY_DEFAULT_TRANSFER_TIMEOUT 0

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
    sentry_path_t *external_crash_reporter;
    sentry_logger_t logger;
    size_t max_breadcrumbs;
    bool debug;
    bool auto_session_tracking;
    bool require_user_consent;
    bool symbolize_stacktraces;
    bool system_crash_reporter_enabled;
    bool attach_screenshot;
    sentry_before_screenshot_function_t before_screenshot_func;
    void *before_screenshot_data;
    bool attach_session_replay;
    uint32_t session_replay_duration;
    bool crashpad_wait_for_upload;
    bool enable_logging_when_crashed;
    bool propagate_traceparent;
    bool strict_trace_continuation;
    bool crashpad_limit_stack_capture_to_sp;
    sentry_cache_keep_t cache_keep;

    time_t cache_max_age;
    size_t cache_max_size;
    size_t cache_max_items;

    sentry_attachment_t *attachments;
    sentry_run_t *run;

    sentry_transport_t *transport;
    sentry_event_function_t before_send_func;
    void *before_send_data;
    sentry_crash_function_t on_crash_func;
    void *on_crash_data;
    sentry_transaction_function_t before_transaction_func;
    void *before_transaction_data;
    sentry_before_send_log_function_t before_send_log_func;
    void *before_send_log_data;
    sentry_before_breadcrumb_function_t before_breadcrumb_func;
    void *before_breadcrumb_data;

    /* Experimentally exposed */
    double traces_sample_rate;
    sentry_traces_sampler_function traces_sampler;
    void *traces_sampler_data;
    char *org_id;
    size_t max_spans;
    bool enable_logs;
    // takes the first varg as a `sentry_value_t` object containing attributes
    // if no custom attributes are to be passed, use `sentry_value_new_object()`
    bool logs_with_attributes;
    bool enable_metrics;
    sentry_before_send_metric_function_t before_send_metric_func;
    void *before_send_metric_data;
    bool enable_app_hang_tracking;
    uint64_t app_hang_timeout;
    bool http_retry;
    bool send_client_reports;
    bool enable_large_attachments;

    /* everything from here on down are options which are stored here but
       not exposed through the options API */
    struct sentry_backend_s *backend;
    sentry_session_t *session;

    long refcount;
    uint64_t shutdown_timeout;
    uint64_t transfer_timeout;
    sentry_handler_strategy_t handler_strategy;
    int minidump_mode; // 0=stack_only, 1=smart, 2=full (see
                       // sentry_crash_context.h)
    int crash_reporting_mode; // 0=minidump, 1=native, 2=native_with_minidump
                              // (see sentry_crash_reporting_mode_t)
    int crash_upload_mode; // 0=sync, 1=async (see sentry_crash_upload_mode_t)

#ifdef SENTRY_PLATFORM_NX
    void (*network_connect_func)(void);
    bool send_default_pii;
#endif
};

/**
 * Increments the reference count and returns the options.
 */
sentry_options_t *sentry__options_incref(sentry_options_t *options);

/**
 * Returns the organization ID used for trace propagation: the `org_id` option
 * if set and non-empty, otherwise the DSN-derived value if non-empty,
 * otherwise NULL.
 */
const char *sentry__options_get_org_id(const sentry_options_t *options);

#endif
