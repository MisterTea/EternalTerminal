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

#include <stdlib.h>
#include <string.h>
#include <winhttp.h>

typedef struct {
    sentry_dsn_t *dsn;
    wchar_t *user_agent;
    wchar_t *proxy;
    sentry_rate_limiter_t *ratelimiter;
    HINTERNET session;
    HINTERNET connect;
    bool debug;
} winhttp_bgworker_state_t;

static winhttp_bgworker_state_t *
sentry__winhttp_bgworker_state_new(void)
{
    winhttp_bgworker_state_t *state = SENTRY_MAKE(winhttp_bgworker_state_t);
    if (!state) {
        return NULL;
    }
    memset(state, 0, sizeof(winhttp_bgworker_state_t));

    state->ratelimiter = sentry__rate_limiter_new();

    return state;
}

static void
sentry__winhttp_bgworker_state_free(void *_state)
{
    winhttp_bgworker_state_t *state = _state;
    if (state->connect) {
        WinHttpCloseHandle(state->connect);
    }
    if (state->session) {
        WinHttpCloseHandle(state->session);
    }
    sentry__dsn_decref(state->dsn);
    sentry__rate_limiter_free(state->ratelimiter);
    sentry_free(state->user_agent);
    sentry_free(state->proxy);
    sentry_free(state);
}

static int
sentry__winhttp_transport_start(
    const sentry_options_t *opts, void *transport_state)
{
    sentry_bgworker_t *bgworker = (sentry_bgworker_t *)transport_state;
    winhttp_bgworker_state_t *state = sentry__bgworker_get_state(bgworker);

    state->dsn = sentry__dsn_incref(opts->dsn);
    state->user_agent = sentry__string_to_wstr(SENTRY_SDK_USER_AGENT);
    state->debug = opts->debug;

    sentry__bgworker_setname(bgworker, opts->transport_thread_name);

    // ensure the proxy starts with `http://`, otherwise ignore it
    if (opts->http_proxy
        && strstr(opts->http_proxy, "http://") == opts->http_proxy) {
        const char *ptr = opts->http_proxy + 7;
        const char *slash = strchr(ptr, '/');
        if (slash) {
            char *copy = sentry__string_clonen(ptr, slash - ptr);
            state->proxy = sentry__string_to_wstr(copy);
            sentry_free(copy);
        } else {
            state->proxy = sentry__string_to_wstr(ptr);
        }
    }

    if (state->proxy) {
        state->session
            = WinHttpOpen(state->user_agent, WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                state->proxy, WINHTTP_NO_PROXY_BYPASS, 0);
    } else {
#if _WIN32_WINNT >= 0x0603
        state->session = WinHttpOpen(state->user_agent,
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
#endif
        // On windows 8.0 or lower, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY does
        // not work on error we fallback to WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
        if (!state->session) {
            state->session = WinHttpOpen(state->user_agent,
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS, 0);
        }
    }
    if (!state->session) {
        SENTRY_WARN("`WinHttpOpen` failed");
        return 1;
    }
    return sentry__bgworker_start(bgworker);
}

static int
sentry__winhttp_transport_shutdown(uint64_t timeout, void *transport_state)
{
    sentry_bgworker_t *bgworker = (sentry_bgworker_t *)transport_state;
    return sentry__bgworker_shutdown(bgworker, timeout);
}

static void
sentry__winhttp_send_task(void *_envelope, void *_state)
{
    sentry_envelope_t *envelope = (sentry_envelope_t *)_envelope;
    winhttp_bgworker_state_t *state = (winhttp_bgworker_state_t *)_state;

    uint64_t started = sentry__monotonic_time();

    sentry_prepared_http_request_t *req = sentry__prepare_http_request(
        envelope, state->dsn, state->ratelimiter);
    if (!req) {
        return;
    }

    wchar_t *url = sentry__string_to_wstr(req->url);
    wchar_t *headers = NULL;
    HINTERNET request = NULL;

    URL_COMPONENTS url_components;
    wchar_t hostname[128];
    wchar_t url_path[4096];
    memset(&url_components, 0, sizeof(URL_COMPONENTS));
    url_components.dwStructSize = sizeof(URL_COMPONENTS);
    url_components.lpszHostName = hostname;
    url_components.dwHostNameLength = 128;
    url_components.lpszUrlPath = url_path;
    url_components.dwUrlPathLength = 1024;

    WinHttpCrackUrl(url, 0, 0, &url_components);
    if (!state->connect) {
        state->connect = WinHttpConnect(state->session,
            url_components.lpszHostName, url_components.nPort, 0);
    }
    if (!state->connect) {
        SENTRY_WARNF("`WinHttpConnect` failed with code `%d`", GetLastError());
        goto exit;
    }

    bool is_secure = strstr(req->url, "https") == req->url;
    request = WinHttpOpenRequest(state->connect, L"POST",
        url_components.lpszUrlPath, NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, is_secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        SENTRY_WARNF(
            "`WinHttpOpenRequest` failed with code `%d`", GetLastError());
        goto exit;
    }

    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);

    for (size_t i = 0; i < req->headers_len; i++) {
        sentry__stringbuilder_append(&sb, req->headers[i].key);
        sentry__stringbuilder_append_char(&sb, ':');
        sentry__stringbuilder_append(&sb, req->headers[i].value);
        sentry__stringbuilder_append(&sb, "\r\n");
    }

    char *headers_buf = sentry__stringbuilder_into_string(&sb);
    headers = sentry__string_to_wstr(headers_buf);
    sentry_free(headers_buf);

    SENTRY_TRACEF(
        "sending request using winhttp to \"%s\":\n%S", req->url, headers);

    if (WinHttpSendRequest(request, headers, (DWORD)-1, (LPVOID)req->body,
            (DWORD)req->body_len, (DWORD)req->body_len, 0)) {
        WinHttpReceiveResponse(request, NULL);

        if (state->debug) {
            // this is basically the example from:
            // https://docs.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpqueryheaders#examples
            DWORD dwSize = 0;
            LPVOID lpOutBuffer = NULL;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                WINHTTP_HEADER_NAME_BY_INDEX, NULL, &dwSize,
                WINHTTP_NO_HEADER_INDEX);

            // Allocate memory for the buffer.
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                lpOutBuffer = sentry_malloc(dwSize);

                // Now, use WinHttpQueryHeaders to retrieve the header.
                if (lpOutBuffer
                    && WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        WINHTTP_HEADER_NAME_BY_INDEX, lpOutBuffer, &dwSize,
                        WINHTTP_NO_HEADER_INDEX)) {
                    SENTRY_TRACEF(
                        "received response:\n%S", (wchar_t *)lpOutBuffer);
                }
                sentry_free(lpOutBuffer);
            }
        }

        // lets just assume we won’t have headers > 2k
        wchar_t buf[2048];
        DWORD buf_size = sizeof(buf);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM,
                L"x-sentry-rate-limits", buf, &buf_size,
                WINHTTP_NO_HEADER_INDEX)) {
            char *h = sentry__string_from_wstr(buf);
            if (h) {
                sentry__rate_limiter_update_from_header(state->ratelimiter, h);
                sentry_free(h);
            }
        } else if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM,
                       L"retry-after", buf, &buf_size,
                       WINHTTP_NO_HEADER_INDEX)) {
            char *h = sentry__string_from_wstr(buf);
            if (h) {
                sentry__rate_limiter_update_from_http_retry_after(
                    state->ratelimiter, h);
                sentry_free(h);
            }
        }
    } else {
        SENTRY_DEBUGF(
            "`WinHttpSendRequest` failed with code `%d`", GetLastError());
    }

    uint64_t now = sentry__monotonic_time();
    SENTRY_TRACEF("request handled in %llums", now - started);

exit:
    if (request) {
        WinHttpCloseHandle(request);
    }
    sentry_free(url);
    sentry_free(headers);
    sentry__prepared_http_request_free(req);
}

static void
sentry__winhttp_transport_send_envelope(
    sentry_envelope_t *envelope, void *transport_state)
{
    sentry_bgworker_t *bgworker = (sentry_bgworker_t *)transport_state;
    sentry__bgworker_submit(bgworker, sentry__winhttp_send_task,
        (void (*)(void *))sentry_envelope_free, envelope);
}

static bool
sentry__winhttp_dump_task(void *envelope, void *run)
{
    sentry__run_write_envelope(
        (sentry_run_t *)run, (sentry_envelope_t *)envelope);
    return true;
}

static size_t
sentry__winhttp_dump_queue(sentry_run_t *run, void *transport_state)
{
    sentry_bgworker_t *bgworker = (sentry_bgworker_t *)transport_state;
    return sentry__bgworker_foreach_matching(
        bgworker, sentry__winhttp_send_task, sentry__winhttp_dump_task, run);
}

sentry_transport_t *
sentry__transport_new_default(void)
{
    SENTRY_DEBUG("initializing winhttp transport");
    winhttp_bgworker_state_t *state = sentry__winhttp_bgworker_state_new();
    if (!state) {
        return NULL;
    }

    sentry_bgworker_t *bgworker
        = sentry__bgworker_new(state, sentry__winhttp_bgworker_state_free);
    if (!bgworker) {
        return NULL;
    }

    sentry_transport_t *transport
        = sentry_transport_new(sentry__winhttp_transport_send_envelope);
    if (!transport) {
        sentry__bgworker_decref(bgworker);
        return NULL;
    }
    sentry_transport_set_state(transport, bgworker);
    sentry_transport_set_free_func(
        transport, (void (*)(void *))sentry__bgworker_decref);
    sentry_transport_set_startup_func(
        transport, sentry__winhttp_transport_start);
    sentry_transport_set_shutdown_func(
        transport, sentry__winhttp_transport_shutdown);
    sentry__transport_set_dump_func(transport, sentry__winhttp_dump_queue);

    return transport;
}
