#include "sentry_options.h"
#include "sentry_alloc.h"
#include "sentry_backend.h"
#include "sentry_database.h"
#include "sentry_logger.h"
#include "sentry_path.h"
#include "sentry_string.h"
#include "sentry_sync.h"
#include "sentry_transport.h"
#include <stdlib.h>

sentry_options_t *
sentry_options_new(void)
{
    sentry_options_t *opts = SENTRY_MAKE(sentry_options_t);
    if (!opts) {
        return NULL;
    }
    memset(opts, 0, sizeof(sentry_options_t));
    opts->database_path = sentry__path_from_str(".sentry-native");
    // we assume the DSN to be ASCII only
    sentry_options_set_dsn(opts, getenv("SENTRY_DSN"));
    const char *debug = getenv("SENTRY_DEBUG");
    opts->debug = debug && sentry__string_eq(debug, "1");
    sentry_logger_t logger = { sentry__logger_defaultlogger, NULL };
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
    opts->user_consent = SENTRY_USER_CONSENT_UNKNOWN;
    opts->auto_session_tracking = true;
    opts->system_crash_reporter_enabled = false;
    opts->symbolize_stacktraces =
    // AIX doesn't have reliable debug IDs for server-side symbolication,
    // and the diversity of Android makes it infeasible to have access to debug
    // files.
#if defined(SENTRY_PLATFORM_ANDROID) || defined(SENTRY_PLATFORM_AIX)
        true;
#else
        false;
#endif
    opts->backend = sentry__backend_new();
    opts->transport = sentry__transport_new_default();
    opts->sample_rate = 1.0;
    opts->refcount = 1;
    opts->shutdown_timeout = SENTRY_DEFAULT_SHUTDOWN_TIMEOUT;
    opts->traces_sample_rate = 0.0;
    opts->max_spans = 0;

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
sentry__attachment_free(sentry_attachment_t *attachment)
{
    sentry__path_free(attachment->path);
    sentry_free(attachment);
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
    sentry_free(opts->http_proxy);
    sentry_free(opts->ca_certs);
    sentry_free(opts->transport_thread_name);
    sentry__path_free(opts->database_path);
    sentry__path_free(opts->handler_path);
    sentry_transport_free(opts->transport);
    sentry__backend_free(opts->backend);

    sentry_attachment_t *next_attachment = opts->attachments;
    while (next_attachment) {
        sentry_attachment_t *attachment = next_attachment;
        next_attachment = attachment->next;

        sentry__attachment_free(attachment);
    }
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

void
sentry_options_set_before_send(
    sentry_options_t *opts, sentry_event_function_t func, void *data)
{
    opts->before_send_func = func;
    opts->before_send_data = data;
}

void
sentry_options_set_on_crash(
    sentry_options_t *opts, sentry_crash_function_t func, void *data)
{
    opts->on_crash_func = func;
    opts->on_crash_data = data;
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
sentry_options_set_sample_rate(sentry_options_t *opts, double sample_rate)
{
    if (sample_rate < 0.0) {
        sample_rate = 0.0;
    } else if (sample_rate > 1.0) {
        sample_rate = 1.0;
    }
    opts->sample_rate = sample_rate;
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
sentry_options_set_http_proxy_n(
    sentry_options_t *opts, const char *proxy, size_t proxy_len)
{
    sentry_free(opts->http_proxy);
    opts->http_proxy = sentry__string_clone_n(proxy, proxy_len);
}

void
sentry_options_set_http_proxy(sentry_options_t *opts, const char *proxy)
{
    sentry_free(opts->http_proxy);
    opts->http_proxy = sentry__string_clone(proxy);
}

const char *
sentry_options_get_http_proxy(const sentry_options_t *opts)
{
    return opts->http_proxy;
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
sentry_options_set_system_crash_reporter_enabled(
    sentry_options_t *opts, int enabled)
{
    opts->system_crash_reporter_enabled = !!enabled;
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

static void
add_attachment(sentry_options_t *opts, sentry_path_t *path)
{
    if (!path) {
        return;
    }
    sentry_attachment_t *attachment = SENTRY_MAKE(sentry_attachment_t);
    if (!attachment) {
        sentry__path_free(path);
        return;
    }
    attachment->path = path;
    attachment->next = opts->attachments;
    opts->attachments = attachment;
}

void
sentry_options_add_attachment(sentry_options_t *opts, const char *path)
{
    add_attachment(opts, sentry__path_from_str(path));
}

void
sentry_options_add_attachment_n(
    sentry_options_t *opts, const char *path, size_t path_len)
{
    add_attachment(opts, sentry__path_from_str_n(path, path_len));
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

#ifdef SENTRY_PLATFORM_WINDOWS
void
sentry_options_add_attachmentw_n(
    sentry_options_t *opts, const wchar_t *path, size_t path_len)
{
    add_attachment(opts, sentry__path_from_wstr_n(path, path_len));
}

void
sentry_options_add_attachmentw(sentry_options_t *opts, const wchar_t *path)
{
    size_t path_len = path ? wcslen(path) : 0;
    sentry_options_add_attachmentw_n(opts, path, path_len);
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

    if (sample_rate < 0.0) {
        sample_rate = 0.0;
    } else if (sample_rate > 1.0) {
        sample_rate = 1.0;
    }
    opts->traces_sample_rate = sample_rate;

    if (sample_rate > 0 && opts->max_spans == 0) {
        opts->max_spans = SENTRY_SPANS_MAX;
    }
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
sentry_options_set_backend(sentry_options_t *opts, sentry_backend_t *backend)
{
    sentry__backend_free(opts->backend);
    opts->backend = backend;
}
