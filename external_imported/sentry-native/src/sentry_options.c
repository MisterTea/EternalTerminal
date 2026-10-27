#include "sentry_options.h"
#include "sentry_alloc.h"
#include "sentry_app_hang_monitor.h"
#include "sentry_attachment.h"
#include "sentry_backend.h"
#include "sentry_database.h"
#include "sentry_logger.h"
#include "sentry_path.h"
#include "sentry_string.h"
#include "sentry_sync.h"
#include "sentry_transport.h"
#include "sentry_utils.h"
#include <math.h>
#include <stdlib.h>

static double
normalize_sample_rate(double sample_rate, double default_value)
{
    if (isnan(sample_rate)) {
        return default_value;
    } else if (sample_rate < 0.0) {
        return 0.0;
    } else if (sample_rate > 1.0) {
        return 1.0;
    }
    return sample_rate;
}

sentry_options_t *
sentry_options_new(void)
{
    sentry_options_t *opts = SENTRY_MAKE(sentry_options_t);
    if (!opts) {
        return NULL;
    }
    opts->database_path = sentry__path_from_str(".sentry-native");
    // we assume the DSN to be ASCII only
    sentry_options_set_dsn(opts, getenv("SENTRY_DSN"));
    const char *debug = getenv("SENTRY_DEBUG");
    opts->debug = debug && sentry__string_eq(debug, "1");
#if !defined(NDEBUG)
    if (!opts->debug && (!debug || !sentry__string_eq(debug, "0"))) {
        opts->debug = 1;
    }
#endif
    sentry_logger_t logger
        = { sentry__logger_defaultlogger, NULL, SENTRY_LEVEL_DEBUG };
    opts->logger = logger;
    opts->transport_thread_name = sentry__string_clone("sentry-http");
#ifdef SENTRY_PLATFORM_WINDOWS
    opts->release = sentry__string_from_wstr(_wgetenv(L"SENTRY_RELEASE"));
    opts->environment
        = sentry__string_from_wstr(_wgetenv(L"SENTRY_ENVIRONMENT"));
#else
    opts->release = sentry__string_clone(getenv("SENTRY_RELEASE"));
    opts->environment = sentry__string_clone(getenv("SENTRY_ENVIRONMENT"));
#endif
    if (!opts->environment) {
        opts->environment = sentry__string_clone("production");
    }
    sentry_options_set_sdk_name(opts, SENTRY_SDK_NAME);
    opts->max_breadcrumbs = SENTRY_BREADCRUMBS_MAX;
    opts->auto_session_tracking = true;
    opts->system_crash_reporter_enabled = false;
    opts->attach_screenshot = false;
    opts->attach_session_replay = false;
    opts->session_replay_duration = 5000;
    opts->crashpad_wait_for_upload = false;
    // On macOS, breakpad suspends all other threads of the process before
    // writing the minidump and invokes our backend callback from inside
    // WriteMinidumpWithException() while those threads are still suspended.
    // Any vfprintf-based log call from that context can deadlock waiting for
    // a stdio lock held by a suspended thread (e.g. libcurl's verbose output
    // on stderr). Default off here; users who accept the risk can still opt
    // in via `sentry_options_set_logger_enabled_when_crashed(opts, 1)`.
#if defined(SENTRY_BACKEND_BREAKPAD) && defined(SENTRY_PLATFORM_DARWIN)        \
    && !defined(SENTRY_PLATFORM_IOS)
    opts->enable_logging_when_crashed = false;
#else
    opts->enable_logging_when_crashed = true;
#endif
    opts->propagate_traceparent = false;
    opts->strict_trace_continuation = false;
    opts->crashpad_limit_stack_capture_to_sp = false;
    opts->enable_metrics = true;
    opts->enable_logs = true;
    opts->cache_keep = SENTRY_CACHE_KEEP_NONE;
    opts->cache_max_age = 0;
    opts->cache_max_size = 0;
    opts->cache_max_items = 30;
    opts->symbolize_stacktraces =
    // AIX doesn't have reliable debug IDs for server-side symbolication,
    // and the diversity of Android makes it infeasible to have access to debug
    // files.
#if defined(SENTRY_PLATFORM_ANDROID) || defined(SENTRY_PLATFORM_AIX)           \
    || defined(SENTRY_PLATFORM_PS)
        true;
#else
        false;
#endif
    opts->backend = sentry__backend_new();
    opts->transport = sentry__transport_new_default();
    opts->refcount = 1;
    opts->shutdown_timeout = SENTRY_DEFAULT_SHUTDOWN_TIMEOUT;
    opts->transfer_timeout = SENTRY_DEFAULT_TRANSFER_TIMEOUT;
    sentry_options_set_sample_rate(
        opts, sentry__getenv_double("SENTRY_SAMPLE_RATE", 1.0));
    sentry_options_set_traces_sample_rate(
        opts, sentry__getenv_double("SENTRY_TRACES_SAMPLE_RATE", 0.0));
    opts->max_spans = SENTRY_SPANS_MAX;
    opts->handler_strategy = SENTRY_HANDLER_STRATEGY_DEFAULT;
    opts->minidump_mode = SENTRY_MINIDUMP_MODE_SMART; // Default: balanced mode
    opts->crash_reporting_mode
        = SENTRY_CRASH_REPORTING_MODE_NATIVE_WITH_MINIDUMP; // Default: best of
                                                            // both worlds
    opts->crash_upload_mode = SENTRY_CRASH_UPLOAD_MODE_SYNC;
    opts->enable_app_hang_tracking = false;
    opts->app_hang_timeout = 5000;
    opts->http_retry = false;
    opts->send_client_reports = true;
    opts->enable_large_attachments = false;

    return opts;
}

sentry_options_t *
sentry__options_incref(sentry_options_t *options)
{
    if (options) {
        sentry__atomic_fetch_and_add(&options->refcount, 1);
    }
    return options;
}

void
sentry_options_free(sentry_options_t *opts)
{
    if (!opts || sentry__atomic_fetch_and_add(&opts->refcount, -1) != 1) {
        return;
    }
    sentry__dsn_decref(opts->dsn);
    sentry_free(opts->release);
    sentry_free(opts->sdk_name);
    sentry_free(opts->user_agent);
    sentry_free(opts->environment);
    sentry_free(opts->dist);
    sentry_free(opts->proxy);
    sentry_free(opts->ca_certs);
    sentry_free(opts->org_id);
    sentry_free(opts->transport_thread_name);
    sentry__path_free(opts->database_path);
    sentry__path_free(opts->handler_path);
    sentry__path_free(opts->external_crash_reporter);
    sentry_transport_free(opts->transport);
    sentry__backend_free(opts->backend);
    sentry__attachments_free(opts->attachments);
    sentry__run_free(opts->run);

    sentry_free(opts);
}

void
sentry_options_set_transport(
    sentry_options_t *opts, sentry_transport_t *transport)
{
    sentry_transport_free(opts->transport);
    opts->transport = transport;
}

#ifdef SENTRY_PLATFORM_NX
void
sentry_options_set_network_connect_func(
    sentry_options_t *opts, void (*network_connect_func)(void))
{
    opts->network_connect_func = network_connect_func;
}

void
sentry_options_set_send_default_pii(sentry_options_t *opts, int value)
{
    opts->send_default_pii = value;
}
#endif

void
sentry_options_set_before_send(
    sentry_options_t *opts, sentry_event_function_t func, void *user_data)
{
    opts->before_send_func = func;
    opts->before_send_data = user_data;
}

void
sentry_options_set_on_crash(
    sentry_options_t *opts, sentry_crash_function_t func, void *user_data)
{
    opts->on_crash_func = func;
    opts->on_crash_data = user_data;
}

void
sentry_options_set_before_transaction(
    sentry_options_t *opts, sentry_transaction_function_t func, void *user_data)
{
    opts->before_transaction_func = func;
    opts->before_transaction_data = user_data;
}

void
sentry_options_set_before_send_log(sentry_options_t *opts,
    sentry_before_send_log_function_t func, void *user_data)
{
    opts->before_send_log_func = func;
    opts->before_send_log_data = user_data;
}

void
sentry_options_set_before_breadcrumb(sentry_options_t *opts,
    sentry_before_breadcrumb_function_t func, void *user_data)
{
    opts->before_breadcrumb_func = func;
    opts->before_breadcrumb_data = user_data;
}

void
sentry_options_set_dsn_n(
    sentry_options_t *opts, const char *raw_dsn, size_t raw_dsn_len)
{
    sentry__dsn_decref(opts->dsn);
    opts->dsn = sentry__dsn_new_n(raw_dsn, raw_dsn_len);
}

void
sentry_options_set_dsn(sentry_options_t *opts, const char *raw_dsn)
{
    sentry__dsn_decref(opts->dsn);
    opts->dsn = sentry__dsn_new(raw_dsn);
}

const char *
sentry_options_get_dsn(const sentry_options_t *opts)
{
    return opts->dsn ? opts->dsn->raw : NULL;
}

void
sentry_options_set_org_id_n(
    sentry_options_t *opts, const char *org_id, size_t org_id_len)
{
    sentry_free(opts->org_id);
    opts->org_id = sentry__string_clone_n(org_id, org_id_len);
}

void
sentry_options_set_org_id(sentry_options_t *opts, const char *org_id)
{
    sentry_free(opts->org_id);
    opts->org_id = sentry__string_clone(org_id);
}

const char *
sentry__options_get_org_id(const sentry_options_t *opts)
{
    if (opts->org_id && *opts->org_id) {
        return opts->org_id;
    }
    if (opts->dsn && opts->dsn->org_id && *opts->dsn->org_id) {
        return opts->dsn->org_id;
    }
    return NULL;
}

void
sentry_options_set_sample_rate(sentry_options_t *opts, double sample_rate)
{
    opts->sample_rate = normalize_sample_rate(sample_rate, 1.0);
}

double
sentry_options_get_sample_rate(const sentry_options_t *opts)
{
    return opts->sample_rate;
}

void
sentry_options_set_release_n(
    sentry_options_t *opts, const char *release, size_t release_len)
{
    sentry_free(opts->release);
    opts->release = sentry__string_clone_n(release, release_len);
}

void
sentry_options_set_release(sentry_options_t *opts, const char *release)
{
    sentry_free(opts->release);
    opts->release = sentry__string_clone(release);
}

const char *
sentry_options_get_release(const sentry_options_t *opts)
{
    return opts->release;
}

void
sentry_options_set_environment_n(
    sentry_options_t *opts, const char *environment, size_t environment_len)
{
    sentry_free(opts->environment);
    opts->environment = sentry__string_clone_n(environment, environment_len);
}

void
sentry_options_set_environment(sentry_options_t *opts, const char *environment)
{
    sentry_free(opts->environment);
    opts->environment = sentry__string_clone(environment);
}

const char *
sentry_options_get_environment(const sentry_options_t *opts)
{
    return opts->environment;
}

void
sentry_options_set_dist_n(
    sentry_options_t *opts, const char *dist, size_t dist_len)
{
    sentry_free(opts->dist);
    opts->dist = sentry__string_clone_n(dist, dist_len);
}

void
sentry_options_set_dist(sentry_options_t *opts, const char *dist)
{
    sentry_free(opts->dist);
    opts->dist = sentry__string_clone(dist);
}

const char *
sentry_options_get_dist(const sentry_options_t *opts)
{
    return opts->dist;
}

void
sentry_options_set_proxy_n(
    sentry_options_t *opts, const char *proxy, size_t proxy_len)
{
    sentry_free(opts->proxy);
    opts->proxy = sentry__string_clone_n(proxy, proxy_len);
}

void
sentry_options_set_proxy(sentry_options_t *opts, const char *proxy)
{
    sentry_free(opts->proxy);
    opts->proxy = sentry__string_clone(proxy);
}

const char *
sentry_options_get_proxy(const sentry_options_t *opts)
{
    return opts->proxy;
}

void
sentry_options_set_http_proxy_n(
    sentry_options_t *opts, const char *proxy, size_t proxy_len)
{
    sentry_options_set_proxy_n(opts, proxy, proxy_len);
}

void
sentry_options_set_http_proxy(sentry_options_t *opts, const char *proxy)
{
    sentry_options_set_proxy(opts, proxy);
}

const char *
sentry_options_get_http_proxy(const sentry_options_t *opts)
{
    return sentry_options_get_proxy(opts);
}

void
sentry_options_set_ca_certs(sentry_options_t *opts, const char *path)
{
    sentry_free(opts->ca_certs);
    opts->ca_certs = sentry__string_clone(path);
}

void
sentry_options_set_ca_certs_n(
    sentry_options_t *opts, const char *path, size_t path_len)
{
    sentry_free(opts->ca_certs);
    opts->ca_certs = sentry__string_clone_n(path, path_len);
}

const char *
sentry_options_get_ca_certs(const sentry_options_t *opts)
{
    return opts->ca_certs;
}

void
sentry_options_set_transport_thread_name(
    sentry_options_t *opts, const char *name)
{
    sentry_free(opts->transport_thread_name);
    opts->transport_thread_name = sentry__string_clone(name);
}

void
sentry_options_set_transport_thread_name_n(
    sentry_options_t *opts, const char *name, size_t name_len)
{
    sentry_free(opts->transport_thread_name);
    opts->transport_thread_name = sentry__string_clone_n(name, name_len);
}

const char *
sentry_options_get_transport_thread_name(const sentry_options_t *opts)
{
    return opts->transport_thread_name;
}

int
sentry_options_set_sdk_name(sentry_options_t *opts, const char *sdk_name)
{
    if (!opts || !sdk_name) {
        return 1;
    }
    const size_t sdk_name_len = strlen(sdk_name);
    return sentry_options_set_sdk_name_n(opts, sdk_name, sdk_name_len);
}

int
sentry_options_set_sdk_name_n(
    sentry_options_t *opts, const char *sdk_name, size_t sdk_name_len)
{
    if (!opts || !sdk_name) {
        return 1;
    }

    sentry_free(opts->sdk_name);
    opts->sdk_name = sentry__string_clone_n(sdk_name, sdk_name_len);

    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);
    sentry__stringbuilder_append(&sb, opts->sdk_name);
    sentry__stringbuilder_append(&sb, "/");
    sentry__stringbuilder_append(&sb, SENTRY_SDK_VERSION);

    sentry_free(opts->user_agent);
    opts->user_agent = sentry__stringbuilder_into_string(&sb);

    return 0;
}

const char *
sentry_options_get_sdk_name(const sentry_options_t *opts)
{
    return opts->sdk_name;
}

const char *
sentry_options_get_user_agent(const sentry_options_t *opts)
{
    return opts->user_agent;
}

void
sentry_options_set_debug(sentry_options_t *opts, int debug)
{
    opts->debug = !!debug;
}

int
sentry_options_get_debug(const sentry_options_t *opts)
{
    return opts->debug;
}

void
sentry_options_set_max_breadcrumbs(
    sentry_options_t *opts, size_t max_breadcrumbs)
{
    opts->max_breadcrumbs = max_breadcrumbs;
}

size_t
sentry_options_get_max_breadcrumbs(const sentry_options_t *opts)
{
    return opts->max_breadcrumbs;
}

void
sentry_options_set_logger(
    sentry_options_t *opts, sentry_logger_function_t func, void *userdata)
{
    opts->logger.logger_func = func;
    opts->logger.logger_data = userdata;
}

void
sentry_options_set_logger_level(sentry_options_t *opts, sentry_level_t level)
{
    opts->logger.logger_level = level;
}

void
sentry_options_set_logger_enabled_when_crashed(sentry_options_t *opts, int val)
{
    opts->enable_logging_when_crashed = !!val;
}

void
sentry_options_set_auto_session_tracking(sentry_options_t *opts, int val)
{
    opts->auto_session_tracking = !!val;
}

int
sentry_options_get_auto_session_tracking(const sentry_options_t *opts)
{
    return opts->auto_session_tracking;
}

void
sentry_options_set_require_user_consent(sentry_options_t *opts, int val)
{
    opts->require_user_consent = !!val;
}

int
sentry_options_get_require_user_consent(const sentry_options_t *opts)
{
    return opts->require_user_consent;
}

void
sentry_options_set_symbolize_stacktraces(sentry_options_t *opts, int val)
{
    opts->symbolize_stacktraces = !!val;
}

int
sentry_options_get_symbolize_stacktraces(const sentry_options_t *opts)
{
    return opts->symbolize_stacktraces;
}

void
sentry_options_set_cache_keep(sentry_options_t *opts, int mode)
{
    if (mode < SENTRY_CACHE_KEEP_NONE) {
        mode = SENTRY_CACHE_KEEP_OFFLINE;
    } else if (mode > SENTRY_CACHE_KEEP_ALWAYS) {
        mode = SENTRY_CACHE_KEEP_ALWAYS;
    }
    opts->cache_keep = (sentry_cache_keep_t)mode;
}

void
sentry_options_set_cache_max_items(sentry_options_t *opts, size_t items)
{
    opts->cache_max_items = items;
}

void
sentry_options_set_cache_max_size(sentry_options_t *opts, size_t bytes)
{
    opts->cache_max_size = bytes;
}

void
sentry_options_set_cache_max_age(sentry_options_t *opts, time_t seconds)
{
    opts->cache_max_age = seconds;
}

int
sentry_options_get_cache_keep(const sentry_options_t *opts)
{
    return opts->cache_keep;
}

void
sentry_options_set_system_crash_reporter_enabled(
    sentry_options_t *opts, int enabled)
{
    opts->system_crash_reporter_enabled = !!enabled;
}

void
sentry_options_set_minidump_mode(
    sentry_options_t *opts, sentry_minidump_mode_t mode)
{
    // Clamp to valid range
    if (mode < SENTRY_MINIDUMP_MODE_STACK_ONLY) {
        mode = SENTRY_MINIDUMP_MODE_STACK_ONLY;
    } else if (mode > SENTRY_MINIDUMP_MODE_FULL) {
        mode = SENTRY_MINIDUMP_MODE_FULL;
    }
    opts->minidump_mode = mode;
}

void
sentry_options_set_crash_reporting_mode(
    sentry_options_t *opts, sentry_crash_reporting_mode_t mode)
{
    // Clamp to valid range (cast to int to handle negative values)
    int imode = (int)mode;
    if (imode < SENTRY_CRASH_REPORTING_MODE_MINIDUMP) {
        imode = SENTRY_CRASH_REPORTING_MODE_MINIDUMP;
    } else if (imode > SENTRY_CRASH_REPORTING_MODE_NATIVE_WITH_MINIDUMP) {
        imode = SENTRY_CRASH_REPORTING_MODE_NATIVE_WITH_MINIDUMP;
    }
    opts->crash_reporting_mode = imode;
}

sentry_crash_reporting_mode_t
sentry_options_get_crash_reporting_mode(const sentry_options_t *opts)
{
    return (sentry_crash_reporting_mode_t)opts->crash_reporting_mode;
}

void
sentry_options_set_crash_upload_mode(
    sentry_options_t *opts, sentry_crash_upload_mode_t mode)
{
    int imode = (int)mode;
    if (imode < SENTRY_CRASH_UPLOAD_MODE_SYNC) {
        imode = SENTRY_CRASH_UPLOAD_MODE_SYNC;
    } else if (imode > SENTRY_CRASH_UPLOAD_MODE_ASYNC) {
        imode = SENTRY_CRASH_UPLOAD_MODE_ASYNC;
    }
    opts->crash_upload_mode = imode;
}

sentry_crash_upload_mode_t
sentry_options_get_crash_upload_mode(const sentry_options_t *opts)
{
    return (sentry_crash_upload_mode_t)opts->crash_upload_mode;
}

void
sentry_options_set_crashpad_wait_for_upload(
    sentry_options_t *opts, int wait_for_upload)
{
    opts->crashpad_wait_for_upload = !!wait_for_upload;
}

void
sentry_options_set_crashpad_limit_stack_capture_to_sp(
    sentry_options_t *opts, int enabled)
{
    opts->crashpad_limit_stack_capture_to_sp = !!enabled;
}

void
sentry_options_set_shutdown_timeout(
    sentry_options_t *opts, uint64_t shutdown_timeout)
{
    opts->shutdown_timeout = shutdown_timeout;
}

uint64_t
sentry_options_get_shutdown_timeout(sentry_options_t *opts)
{
    return opts->shutdown_timeout;
}

void
sentry_options_set_transfer_timeout(
    sentry_options_t *opts, uint64_t transfer_timeout)
{
    opts->transfer_timeout = transfer_timeout;
}

uint64_t
sentry_options_get_transfer_timeout(sentry_options_t *opts)
{
    return opts->transfer_timeout;
}

void
sentry_options_add_attachment(sentry_options_t *opts, const char *path)
{
    sentry__attachments_add_path(
        &opts->attachments, sentry__path_from_str(path), NULL, NULL);
}

void
sentry_options_add_attachment_n(
    sentry_options_t *opts, const char *path, size_t path_len)
{
    sentry__attachments_add_path(&opts->attachments,
        sentry__path_from_str_n(path, path_len), NULL, NULL);
}

void
sentry_options_add_view_hierarchy(sentry_options_t *opts, const char *path)
{
    sentry__attachments_add_path(&opts->attachments,
        sentry__path_from_str(path), SENTRY_ATTACHMENT_TYPE_VIEW_HIERARCHY,
        "application/json");
}

void
sentry_options_add_view_hierarchy_n(
    sentry_options_t *opts, const char *path, size_t path_len)
{
    sentry__attachments_add_path(&opts->attachments,
        sentry__path_from_str_n(path, path_len),
        SENTRY_ATTACHMENT_TYPE_VIEW_HIERARCHY, "application/json");
}

void
sentry_options_set_attach_screenshot(sentry_options_t *opts, int val)
{
    opts->attach_screenshot = !!val;
}

void
sentry_options_set_before_screenshot(sentry_options_t *opts,
    sentry_before_screenshot_function_t func, void *user_data)
{
    opts->before_screenshot_func = func;
    opts->before_screenshot_data = user_data;
}

void
sentry_options_set_attach_session_replay(sentry_options_t *opts, int val)
{
    opts->attach_session_replay = !!val;
}

void
sentry_options_set_session_replay_duration(
    sentry_options_t *opts, uint32_t duration_ms)
{
    opts->session_replay_duration = duration_ms;
}

void
sentry_options_set_handler_path(sentry_options_t *opts, const char *path)
{
    sentry__path_free(opts->handler_path);
    opts->handler_path = sentry__path_from_str(path);
}

void
sentry_options_set_handler_path_n(
    sentry_options_t *opts, const char *path, size_t path_len)
{
    sentry__path_free(opts->handler_path);
    opts->handler_path = sentry__path_from_str_n(path, path_len);
}

void
sentry_options_set_database_path(sentry_options_t *opts, const char *path)
{
    sentry__path_free(opts->database_path);
    opts->database_path = sentry__path_from_str(path);
}

void
sentry_options_set_database_path_n(
    sentry_options_t *opts, const char *path, size_t path_len)
{
    sentry__path_free(opts->database_path);
    opts->database_path = sentry__path_from_str_n(path, path_len);
}

void
sentry_options_set_external_crash_reporter_path(
    sentry_options_t *opts, const char *path)
{
    sentry__path_free(opts->external_crash_reporter);
    opts->external_crash_reporter = sentry__path_from_str(path);
}

void
sentry_options_set_external_crash_reporter_path_n(
    sentry_options_t *opts, const char *path, size_t path_len)
{
    sentry__path_free(opts->external_crash_reporter);
    opts->external_crash_reporter = sentry__path_from_str_n(path, path_len);
}

#ifdef SENTRY_PLATFORM_WINDOWS
void
sentry_options_add_attachmentw_n(
    sentry_options_t *opts, const wchar_t *path, size_t path_len)
{
    sentry__attachments_add_path(&opts->attachments,
        sentry__path_from_wstr_n(path, path_len), NULL, NULL);
}

void
sentry_options_add_attachmentw(sentry_options_t *opts, const wchar_t *path)
{
    size_t path_len = path ? wcslen(path) : 0;
    sentry_options_add_attachmentw_n(opts, path, path_len);
}

void
sentry_options_add_view_hierarchyw(sentry_options_t *opts, const wchar_t *path)
{
    size_t path_len = path ? wcslen(path) : 0;
    sentry__attachments_add_path(&opts->attachments,
        sentry__path_from_wstr_n(path, path_len),
        SENTRY_ATTACHMENT_TYPE_VIEW_HIERARCHY, "application/json");
}

void
sentry_options_add_view_hierarchyw_n(
    sentry_options_t *opts, const wchar_t *path, size_t path_len)
{
    sentry__attachments_add_path(&opts->attachments,
        sentry__path_from_wstr_n(path, path_len),
        SENTRY_ATTACHMENT_TYPE_VIEW_HIERARCHY, "application/json");
}

void
sentry_options_set_handler_pathw_n(
    sentry_options_t *opts, const wchar_t *path, size_t path_len)
{
    sentry__path_free(opts->handler_path);
    opts->handler_path = sentry__path_from_wstr_n(path, path_len);
}

void
sentry_options_set_handler_pathw(sentry_options_t *opts, const wchar_t *path)
{
    size_t path_len = path ? wcslen(path) : 0;
    sentry_options_set_handler_pathw_n(opts, path, path_len);
}

void
sentry_options_set_external_crash_reporter_pathw_n(
    sentry_options_t *opts, const wchar_t *path, size_t path_len)
{
    sentry__path_free(opts->external_crash_reporter);
    opts->external_crash_reporter = sentry__path_from_wstr_n(path, path_len);
}

void
sentry_options_set_external_crash_reporter_pathw(
    sentry_options_t *opts, const wchar_t *path)
{
    size_t path_len = path ? wcslen(path) : 0;
    sentry_options_set_external_crash_reporter_pathw_n(opts, path, path_len);
}

void
sentry_options_set_database_pathw_n(
    sentry_options_t *opts, const wchar_t *path, size_t path_len)
{
    sentry__path_free(opts->database_path);
    opts->database_path = sentry__path_from_wstr_n(path, path_len);
}

void
sentry_options_set_database_pathw(sentry_options_t *opts, const wchar_t *path)
{
    size_t path_len = path ? wcslen(path) : 0;
    sentry_options_set_database_pathw_n(opts, path, path_len);
}
#endif

/**
 * Sets the maximum number of spans that can be attached to a
 * transaction.
 */
void
sentry_options_set_max_spans(sentry_options_t *opts, size_t max_spans)
{
    opts->max_spans = max_spans;
}

/**
 * Gets the maximum number of spans that can be attached to a
 * transaction.
 */
size_t
sentry_options_get_max_spans(sentry_options_t *opts)
{
    return opts->max_spans;
}

/**
 * Sets the sample rate for transactions. Should be a double between
 * `0.0` and `1.0`. Transactions will be randomly discarded during
 * `sentry_transaction_finish` when the sample rate is < 1.0.
 */
void
sentry_options_set_traces_sample_rate(
    sentry_options_t *opts, double sample_rate)
{
    opts->traces_sample_rate = normalize_sample_rate(sample_rate, 0.0);
}

/**
 * Returns the sample rate for transactions.
 */
double
sentry_options_get_traces_sample_rate(sentry_options_t *opts)
{
    return opts->traces_sample_rate;
}

void
sentry_options_set_traces_sampler(sentry_options_t *opts,
    sentry_traces_sampler_function callback, void *user_data)
{
    opts->traces_sampler = callback;
    opts->traces_sampler_data = user_data;
}

void
sentry_options_set_backend(sentry_options_t *opts, sentry_backend_t *backend)
{
    sentry__backend_free(opts->backend);
    opts->backend = backend;
}

void
sentry_options_set_enable_logs(sentry_options_t *opts, int enable_logs)
{
    opts->enable_logs = !!enable_logs;
}

int
sentry_options_get_enable_logs(const sentry_options_t *opts)
{
    return opts->enable_logs;
}

void
sentry_options_set_logs_with_attributes(
    sentry_options_t *opts, int logs_with_attributes)
{
    opts->logs_with_attributes = !!logs_with_attributes;
}

int
sentry_options_get_logs_with_attributes(const sentry_options_t *opts)
{
    return opts->logs_with_attributes;
}

void
sentry_options_set_enable_metrics(sentry_options_t *opts, int enable_metrics)
{
    opts->enable_metrics = !!enable_metrics;
}

int
sentry_options_get_enable_metrics(const sentry_options_t *opts)
{
    return opts->enable_metrics;
}

void
sentry_options_set_enable_app_hang_tracking(sentry_options_t *opts, int enable)
{
    opts->enable_app_hang_tracking = !!enable;
}

int
sentry_options_get_enable_app_hang_tracking(const sentry_options_t *opts)
{
    return opts->enable_app_hang_tracking;
}

void
sentry_options_set_app_hang_timeout(
    sentry_options_t *opts, uint64_t app_hang_timeout)
{
    // Clamp up to the smallest timeout the watchdog can resolve (see
    // SENTRY_APP_HANG_MIN_TIMEOUT_MS).
    if (app_hang_timeout < SENTRY_APP_HANG_MIN_TIMEOUT_MS) {
        SENTRY_WARNF("app-hang: `app_hang_timeout` of %" PRIu64
                     "ms is below the minimum of %dms, clamping",
            app_hang_timeout, SENTRY_APP_HANG_MIN_TIMEOUT_MS);
        app_hang_timeout = SENTRY_APP_HANG_MIN_TIMEOUT_MS;
    }
    opts->app_hang_timeout = app_hang_timeout;
}

uint64_t
sentry_options_get_app_hang_timeout(const sentry_options_t *opts)
{
    return opts->app_hang_timeout;
}

void
sentry_options_set_enable_large_attachments(
    sentry_options_t *opts, int enable_large_attachments)
{
    opts->enable_large_attachments = !!enable_large_attachments;
}

int
sentry_options_get_enable_large_attachments(const sentry_options_t *opts)
{
    return opts->enable_large_attachments;
}

void
sentry_options_set_before_send_metric(sentry_options_t *opts,
    sentry_before_send_metric_function_t func, void *user_data)
{
    opts->before_send_metric_func = func;
    opts->before_send_metric_data = user_data;
}

#ifdef SENTRY_PLATFORM_LINUX

sentry_handler_strategy_t
sentry_options_get_handler_strategy(const sentry_options_t *opts)
{
    return opts->handler_strategy;
}

void
sentry_options_set_handler_strategy(
    sentry_options_t *opts, sentry_handler_strategy_t handler_strategy)
{
    opts->handler_strategy = handler_strategy;
}

#endif // SENTRY_PLATFORM_LINUX

void
sentry_options_set_http_retry(sentry_options_t *opts, int enabled)
{
    opts->http_retry = !!enabled;
}

int
sentry_options_get_http_retry(const sentry_options_t *opts)
{
    return opts->http_retry;
}

void
sentry_options_set_propagate_traceparent(
    sentry_options_t *opts, int propagate_traceparent)
{
    opts->propagate_traceparent = !!propagate_traceparent;
}

int
sentry_options_get_propagate_traceparent(const sentry_options_t *opts)
{
    return opts->propagate_traceparent;
}

void
sentry_options_set_strict_trace_continuation(
    sentry_options_t *opts, int strict_trace_continuation)
{
    opts->strict_trace_continuation = !!strict_trace_continuation;
}

int
sentry_options_get_strict_trace_continuation(const sentry_options_t *opts)
{
    return opts->strict_trace_continuation;
}

void
sentry_options_set_send_client_reports(sentry_options_t *opts, int val)
{
    opts->send_client_reports = !!val;
}

int
sentry_options_get_send_client_reports(const sentry_options_t *opts)
{
    return opts->send_client_reports;
}
