#include "sentry_alloc.h"
#include "sentry_core.h"
#include "sentry_database.h"
#include "sentry_envelope.h"
#include "sentry_options.h"
#include "sentry_ratelimiter.h"
#include "sentry_string.h"
#include "sentry_sync.h"
#include "sentry_transport.h"
#include "sentry_utils.h"

#include <curl/curl.h>
#include <curl/easy.h>
#include <string.h>

#ifdef SENTRY_PLATFORM_NX
#    include "sentry_transport_curl_nx.h"
#endif

typedef struct curl_transport_state_s {
    sentry_dsn_t *dsn;
    CURL *curl_handle;
    char *user_agent;
    char *proxy;
    char *ca_certs;
    sentry_rate_limiter_t *ratelimiter;
    bool debug;
#ifdef SENTRY_PLATFORM_NX
    void *nx_state;
#endif
} curl_bgworker_state_t;

struct header_info {
    char *x_sentry_rate_limits;
    char *retry_after;
};

static curl_bgworker_state_t *
sentry__curl_bgworker_state_new(void)
{
    curl_bgworker_state_t *state = SENTRY_MAKE(curl_bgworker_state_t);
    if (!state) {
        return NULL;
    }
    memset(state, 0, sizeof(curl_bgworker_state_t));

    state->ratelimiter = sentry__rate_limiter_new();
#ifdef SENTRY_PLATFORM_NX
    state->nx_state = sentry_nx_curl_state_new();
#endif
    return state;
}

static void
sentry__curl_bgworker_state_free(void *_state)
{
    curl_bgworker_state_t *state = _state;
    if (state->curl_handle) {
        curl_easy_cleanup(state->curl_handle);
        curl_global_cleanup();
    }
    sentry__dsn_decref(state->dsn);
    sentry__rate_limiter_free(state->ratelimiter);
    sentry_free(state->ca_certs);
    sentry_free(state->user_agent);
    sentry_free(state->proxy);
#ifdef SENTRY_PLATFORM_NX
    sentry_nx_curl_state_free(state->nx_state);
#endif
    sentry_free(state);
}

static int
sentry__curl_transport_start(
    const sentry_options_t *options, void *transport_state)
{
    static bool curl_initialized = false;
    if (!curl_initialized) {
        CURLcode rv = curl_global_init(CURL_GLOBAL_ALL);
        if (rv != CURLE_OK) {
            SENTRY_WARNF("`curl_global_init` failed with code `%d`", (int)rv);
            return 1;
        }

        curl_version_info_data *version_data
            = curl_version_info(CURLVERSION_NOW);

        if (!version_data) {
            SENTRY_WARN("Failed to retrieve `curl_version_info`");
            return 1;
        }

        sentry_version_t curl_version = {
            .major = (version_data->version_num >> 16) & 0xff,
            .minor = (version_data->version_num >> 8) & 0xff,
            .patch = version_data->version_num & 0xff,
        };

        if (!sentry__check_min_version(
                curl_version, (sentry_version_t) { 7, 21, 7 })) {
            SENTRY_WARNF("`libcurl` is at unsupported version `%u.%u.%u`",
                curl_version.major, curl_version.minor, curl_version.patch);
            return 1;
        }

        if ((version_data->features & CURL_VERSION_ASYNCHDNS) == 0) {
            SENTRY_WARN("`libcurl` was not compiled with feature `AsynchDNS`");
            return 1;
        }
    }

    sentry_bgworker_t *bgworker = (sentry_bgworker_t *)transport_state;
    curl_bgworker_state_t *state = sentry__bgworker_get_state(bgworker);

    state->dsn = sentry__dsn_incref(options->dsn);
    state->proxy = sentry__string_clone(options->proxy);
    state->user_agent = sentry__string_clone(options->user_agent);
    state->ca_certs = sentry__string_clone(options->ca_certs);
    state->curl_handle = curl_easy_init();
    state->debug = options->debug;

    sentry__bgworker_setname(bgworker, options->transport_thread_name);

    if (!state->curl_handle) {
        // In this case we don’t start the worker at all, which means we can
        // still dump all unsent envelopes to disk on shutdown.
        SENTRY_WARN("`curl_easy_init` failed");
        return 1;
    }

#ifdef SENTRY_PLATFORM_NX
    if (!sentry_nx_transport_start(state->nx_state, options)) {
        return 1;
    }
#endif

    return sentry__bgworker_start(bgworker);
}

static int
sentry__curl_transport_flush(uint64_t timeout, void *transport_state)
{
    sentry_bgworker_t *bgworker = (sentry_bgworker_t *)transport_state;
    return sentry__bgworker_flush(bgworker, timeout);
}

static int
sentry__curl_transport_shutdown(uint64_t timeout, void *transport_state)
{
    sentry_bgworker_t *bgworker = (sentry_bgworker_t *)transport_state;
    return sentry__bgworker_shutdown(bgworker, timeout);
}

static size_t
swallow_data(
    char *UNUSED(ptr), size_t size, size_t nmemb, void *UNUSED(userdata))
{
    return size * nmemb;
}

static size_t
header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
    size_t bytes = size * nitems;
    struct header_info *info = userdata;
    char *header = sentry__string_clone_n(buffer, bytes);
    if (!header) {
        return bytes;
    }

    char *sep = strchr(header, ':');
    if (sep) {
        *sep = '\0';
        sentry__string_ascii_lower(header);
        if (sentry__string_eq(header, "retry-after")) {
            info->retry_after = sentry__string_clone(sep + 1);
        } else if (sentry__string_eq(header, "x-sentry-rate-limits")) {
            info->x_sentry_rate_limits = sentry__string_clone(sep + 1);
        }
    }

    sentry_free(header);
    return bytes;
}

static void
sentry__curl_send_task(void *_envelope, void *_state)
{
    sentry_envelope_t *envelope = (sentry_envelope_t *)_envelope;
    curl_bgworker_state_t *state = (curl_bgworker_state_t *)_state;

#ifdef SENTRY_PLATFORM_NX
    if (!sentry_nx_curl_connect(state->nx_state)) {
        return; // TODO should we dump the envelope to disk?
    }
#endif

    sentry_prepared_http_request_t *req = sentry__prepare_http_request(
        envelope, state->dsn, state->ratelimiter, state->user_agent);
    if (!req) {
        return;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "expect:");
    for (size_t i = 0; i < req->headers_len; i++) {
        char buf[512];
        size_t written = (size_t)snprintf(buf, sizeof(buf), "%s:%s",
            req->headers[i].key, req->headers[i].value);
        if (written >= sizeof(buf)) {
            continue;
        }
        buf[written] = '\0';
        headers = curl_slist_append(headers, buf);
    }

    CURL *curl = state->curl_handle;
    curl_easy_reset(curl);
    if (state->debug) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, stderr);
        // CURLOPT_WRITEFUNCTION will `fwrite` by default
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, swallow_data);
    }
    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_POST, (long)1);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req->body_len);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, SENTRY_SDK_USER_AGENT);

    char error_buf[CURL_ERROR_SIZE];
    error_buf[0] = 0;
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buf);

    struct header_info info;
    info.retry_after = NULL;
    info.x_sentry_rate_limits = NULL;
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)&info);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);

    if (state->proxy) {
        curl_easy_setopt(curl, CURLOPT_PROXY, state->proxy);
    }
    if (state->ca_certs) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, state->ca_certs);
    }

#ifdef SENTRY_PLATFORM_NX
    CURLcode rv = sentry_nx_curl_easy_setopt(state->nx_state, curl, req);
#else
    CURLcode rv = CURLE_OK;
#endif

    if (rv == CURLE_OK) {
        rv = curl_easy_perform(curl);
    }

    if (rv == CURLE_OK) {
        long response_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

        if (info.x_sentry_rate_limits) {
            sentry__rate_limiter_update_from_header(
                state->ratelimiter, info.x_sentry_rate_limits);
        } else if (info.retry_after) {
            sentry__rate_limiter_update_from_http_retry_after(
                state->ratelimiter, info.retry_after);
        } else if (response_code == 429) {
            sentry__rate_limiter_update_from_429(state->ratelimiter);
        }
    } else {
        size_t len = strlen(error_buf);
        if (len) {
            if (error_buf[len - 1] == '\n') {
                error_buf[len - 1] = 0;
            }
            SENTRY_WARNF("`curl_easy_perform` failed with code `%d`: %s",
                (int)rv, error_buf);
        } else {
            SENTRY_WARNF("`curl_easy_perform` failed with code `%d`: %s",
                (int)rv, curl_easy_strerror(rv));
        }
    }

    curl_slist_free_all(headers);
    sentry_free(info.retry_after);
    sentry_free(info.x_sentry_rate_limits);
    sentry__prepared_http_request_free(req);
}

static void
sentry__curl_transport_send_envelope(
    sentry_envelope_t *envelope, void *transport_state)
{
    sentry_bgworker_t *bgworker = (sentry_bgworker_t *)transport_state;
    sentry__bgworker_submit(bgworker, sentry__curl_send_task,
        (void (*)(void *))sentry_envelope_free, envelope);
}

static bool
sentry__curl_dump_task(void *envelope, void *run)
{
    sentry__run_write_envelope(
        (sentry_run_t *)run, (sentry_envelope_t *)envelope);
    return true;
}

size_t
sentry__curl_dump_queue(sentry_run_t *run, void *transport_state)
{
    sentry_bgworker_t *bgworker = (sentry_bgworker_t *)transport_state;
    return sentry__bgworker_foreach_matching(
        bgworker, sentry__curl_send_task, sentry__curl_dump_task, run);
}

sentry_transport_t *
sentry__transport_new_default(void)
{
    SENTRY_INFO("initializing curl transport");
    curl_bgworker_state_t *state = sentry__curl_bgworker_state_new();
    if (!state) {
        return NULL;
    }

    sentry_bgworker_t *bgworker
        = sentry__bgworker_new(state, sentry__curl_bgworker_state_free);
    if (!bgworker) {
        return NULL;
    }

    sentry_transport_t *transport
        = sentry_transport_new(sentry__curl_transport_send_envelope);
    if (!transport) {
        sentry__bgworker_decref(bgworker);
        return NULL;
    }
    sentry_transport_set_state(transport, bgworker);
    sentry_transport_set_free_func(
        transport, (void (*)(void *))sentry__bgworker_decref);
    sentry_transport_set_startup_func(transport, sentry__curl_transport_start);
    sentry_transport_set_flush_func(transport, sentry__curl_transport_flush);
    sentry_transport_set_shutdown_func(
        transport, sentry__curl_transport_shutdown);
    sentry__transport_set_dump_func(transport, sentry__curl_dump_queue);

    return transport;
}
