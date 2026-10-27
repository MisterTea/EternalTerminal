#include "sentry_alloc.h"
#include "sentry_core.h"
#include "sentry_envelope.h"
#include "sentry_http_transport.h"
#include "sentry_options.h"
#include "sentry_string.h"
#include "sentry_sync.h"
#include "sentry_transport.h"
#include "sentry_utils.h"

#ifdef SENTRY_PLATFORM_XBOX
#    include "sentry_transport_xbox.h"
#endif

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <winhttp.h>

typedef struct {
    wchar_t *proxy;
    wchar_t *proxy_username;
    wchar_t *proxy_password;
    HINTERNET session;
    HINTERNET connect;
    HINTERNET request;
    bool debug;
    uint64_t transfer_timeout;
    long shutdown;
} winhttp_client_t;

static winhttp_client_t *
winhttp_client_new(void)
{
    winhttp_client_t *client = SENTRY_MAKE(winhttp_client_t);
    if (!client) {
        return NULL;
    }

    return client;
}

static void
winhttp_client_free(void *_client)
{
    winhttp_client_t *client = _client;
    if (client->connect) {
        WinHttpCloseHandle(client->connect);
    }
    if (client->session) {
        WinHttpCloseHandle(client->session);
    }
    sentry_free(client->proxy_username);
    sentry_free(client->proxy_password);
    sentry_free(client->proxy);
    sentry_free(client);
}

// Function to extract and set credentials
static void
set_proxy_credentials(winhttp_client_t *state, const char *proxy)
{
    sentry_url_t url;
    sentry__url_parse(&url, proxy, false);
    if (url.username && url.password) {
        // Convert user and pass to LPCWSTR
        int user_wlen
            = MultiByteToWideChar(CP_UTF8, 0, url.username, -1, NULL, 0);
        int pass_wlen
            = MultiByteToWideChar(CP_UTF8, 0, url.password, -1, NULL, 0);
        wchar_t *user_w
            = (wchar_t *)malloc((size_t)user_wlen * sizeof(wchar_t));
        wchar_t *pass_w
            = (wchar_t *)malloc((size_t)pass_wlen * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, url.username, -1, user_w, user_wlen);
        MultiByteToWideChar(CP_UTF8, 0, url.password, -1, pass_w, pass_wlen);

        state->proxy_username = user_w;
        state->proxy_password = pass_w;
    }
    sentry__url_cleanup(&url);
}

static int
winhttp_timeout_ms(uint64_t timeout)
{
    return timeout > (uint64_t)INT_MAX ? INT_MAX : (int)timeout;
}

static int
winhttp_client_start(void *_client, const sentry_options_t *opts)
{
    winhttp_client_t *client = _client;

    wchar_t *user_agent = sentry__string_to_wstr(opts->user_agent);
    client->debug = opts->debug;
    client->transfer_timeout = opts->transfer_timeout;

    const char *env_proxy = opts->dsn
        ? getenv(opts->dsn->is_secure ? "https_proxy" : "http_proxy")
        : NULL;
    const char *proxy = opts->proxy ? opts->proxy : env_proxy ? env_proxy : "";

    // ensure the proxy starts with `http://`, otherwise ignore it
    if (proxy && strstr(proxy, "http://") == proxy) {
        const char *ptr = proxy + 7;
        const char *at_sign = strchr(ptr, '@');
        const char *slash = strchr(ptr, '/');
        if (at_sign && (!slash || at_sign < slash)) {
            ptr = at_sign + 1;
            set_proxy_credentials(client, proxy);
        }
        if (slash) {
            char *copy = sentry__string_clone_n(ptr, (size_t)(slash - ptr));
            client->proxy = sentry__string_to_wstr(copy);
            sentry_free(copy);
        } else {
            client->proxy = sentry__string_to_wstr(ptr);
        }
    }

    if (client->proxy) {
        client->session
            = WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                client->proxy, WINHTTP_NO_PROXY_BYPASS, 0);
    } else {
#if _WIN32_WINNT >= 0x0603
        client->session
            = WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#endif
        // On windows 8.0 or lower, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY does
        // not work on error we fallback to WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
        if (!client->session) {
            client->session
                = WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        }
    }
    sentry_free(user_agent);

    if (!client->session) {
        SENTRY_WARN("`WinHttpOpen` failed");
        return 1;
    }

    // 15s for resolve/connect, transfer_timeout for send/receive per packet
    int transfer_timeout = winhttp_timeout_ms(client->transfer_timeout);
    WinHttpSetTimeouts(
        client->session, 15000, 15000, transfer_timeout, transfer_timeout);

    return 0;
}

static void
winhttp_client_shutdown(void *_client)
{
    winhttp_client_t *client = _client;
    sentry__atomic_store(&client->shutdown, 1);
    // Seems like some requests are taking too long/hanging
    // Just close them to make sure the background thread is exiting.
    if (client->connect) {
        WinHttpCloseHandle(client->connect);
        client->connect = NULL;
    }

    // NOTE: We need to close the session before closing the request.
    // This will cancel all other requests which might be queued as well.
    if (client->session) {
        WinHttpCloseHandle(client->session);
        client->session = NULL;
    }
    HINTERNET request = InterlockedExchangePointer(&client->request, NULL);
    if (request) {
        WinHttpCloseHandle(request);
    }
}

static char *
query_header(HINTERNET request, const wchar_t *header)
{
    // lets just assume we won't have headers > 2k
    wchar_t buf[2048];
    DWORD buf_size = sizeof(buf);

    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, header, buf,
            &buf_size, WINHTTP_NO_HEADER_INDEX)) {
        return sentry__string_from_wstr(buf);
    }

    return NULL;
}

static bool
winhttp_send_task(void *_client, sentry_prepared_http_request_t *req,
    sentry_http_response_t *resp)
{
    winhttp_client_t *client = (winhttp_client_t *)_client;
    bool result = false;

    uint64_t started = sentry__monotonic_time();

    wchar_t *url = sentry__string_to_wstr(req->url);
    wchar_t *headers = NULL;

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

#ifdef SENTRY_PLATFORM_XBOX
    // Ensure Xbox network connectivity is initialized before HTTP requests
    if (!sentry__xbox_ensure_network_initialized()) {
        SENTRY_WARN("Xbox: Network not ready, skipping HTTP request");
        goto exit;
    }
#endif

    if (!client->connect) {
        client->connect = WinHttpConnect(client->session,
            url_components.lpszHostName, url_components.nPort, 0);
    }
    if (!client->connect) {
        SENTRY_WARNF("`WinHttpConnect` failed with code `%d`", GetLastError());
        goto exit;
    }

    bool is_secure = strstr(req->url, "https") == req->url;
    wchar_t *method_w = sentry__string_to_wstr(req->method);
    client->request = WinHttpOpenRequest(client->connect, method_w,
        url_components.lpszUrlPath, NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, is_secure ? WINHTTP_FLAG_SECURE : 0);
    sentry_free(method_w);
    if (!client->request) {
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

    if (headers_buf) {
        SENTRY_DEBUGF("sending request using winhttp to \"%s\":\n%s", req->url,
            headers_buf);
    }
    sentry_free(headers_buf);

    if (!headers) {
        SENTRY_WARN("winhttp_send_task: failed to allocate headers");
        goto exit;
    }

    if (client->proxy_username && client->proxy_password) {
        WinHttpSetCredentials(client->request, WINHTTP_AUTH_TARGET_PROXY,
            WINHTTP_AUTH_SCHEME_BASIC, client->proxy_username,
            client->proxy_password, 0);
    }

    if (req->body_path) {
        HANDLE hFile = CreateFileW(req->body_path->path_w, GENERIC_READ,
            FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            SENTRY_WARNF("failed to open request body file \"%s\"",
                sentry__path_filename(req->body_path));
            goto exit;
        }

        // https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpsendrequest#support-for-greater-than-4-gb-upload
        DWORD total_length = req->body_len > (size_t)(DWORD)-1
            ? WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH
            : (DWORD)req->body_len;
        result = WinHttpSendRequest(client->request, headers, (DWORD)-1,
            WINHTTP_NO_REQUEST_DATA, 0, total_length, 0);
        if (result) {
            char chunk[65536];
            DWORD bytes_read = 0;
            while (true) {
                if (!ReadFile(hFile, chunk, sizeof(chunk), &bytes_read, NULL)) {
                    SENTRY_WARNF("failed to read request body file \"%s\" "
                                 "with code `%d`",
                        sentry__path_filename(req->body_path), GetLastError());
                    result = false;
                    break;
                }
                if (bytes_read == 0) {
                    break;
                }

                DWORD bytes_written = 0;
                if (!WinHttpWriteData(
                        client->request, chunk, bytes_read, &bytes_written)) {
                    SENTRY_WARNF("failed to upload request body file \"%s\" "
                                 "with code `%d`",
                        sentry__path_filename(req->body_path), GetLastError());
                    result = false;
                    break;
                }
                if (bytes_written != bytes_read) {
                    SENTRY_WARNF("failed to upload request body file \"%s\"",
                        sentry__path_filename(req->body_path));
                    result = false;
                    break;
                }
            }
        } else {
            SENTRY_WARNF(
                "`WinHttpSendRequest` failed with code `%d`", GetLastError());
        }
        CloseHandle(hFile);
    } else {
        result = WinHttpSendRequest(client->request, headers, (DWORD)-1,
            (LPVOID)req->body, (DWORD)req->body_len, (DWORD)req->body_len, 0);
        if (!result) {
            SENTRY_WARNF(
                "`WinHttpSendRequest` failed with code `%d`", GetLastError());
        }
    }

    if (result) {
        if (!(result = WinHttpReceiveResponse(client->request, NULL))) {
            SENTRY_WARNF("`WinHttpReceiveResponse` failed with code `%d`",
                GetLastError());
            goto exit;
        }

        if (client->debug) {
            // this is basically the example from:
            // https://docs.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpqueryheaders#examples
            DWORD dwSize = 0;
            LPVOID lpOutBuffer = NULL;
            WinHttpQueryHeaders(client->request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                WINHTTP_HEADER_NAME_BY_INDEX, NULL, &dwSize,
                WINHTTP_NO_HEADER_INDEX);

            // Allocate memory for the buffer.
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                lpOutBuffer = sentry_malloc(dwSize);

                // Now, use WinHttpQueryHeaders to retrieve the header.
                if (lpOutBuffer
                    && WinHttpQueryHeaders(client->request,
                        WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        WINHTTP_HEADER_NAME_BY_INDEX, lpOutBuffer, &dwSize,
                        WINHTTP_NO_HEADER_INDEX)) {
                    SENTRY_DEBUGF(
                        "received response:\n%S", (wchar_t *)lpOutBuffer);
                }
                sentry_free(lpOutBuffer);
            }
        }

        DWORD status_code = 0;
        DWORD status_code_size = sizeof(status_code);

        WinHttpQueryHeaders(client->request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_code_size,
            WINHTTP_NO_HEADER_INDEX);
        resp->status_code = (int)status_code;

        resp->x_sentry_rate_limits
            = query_header(client->request, L"x-sentry-rate-limits");
        if (!resp->x_sentry_rate_limits) {
            resp->retry_after = query_header(client->request, L"retry-after");
        }

        resp->location = query_header(client->request, L"location");
    }

    uint64_t now = sentry__monotonic_time();
    SENTRY_DEBUGF("request handled in %llums", now - started);

exit:;
    if (!result && sentry__atomic_fetch(&client->shutdown)) {
        resp->shutdown = true;
    }
    HINTERNET request = InterlockedExchangePointer(&client->request, NULL);
    if (request) {
        WinHttpCloseHandle(request);
    }
    sentry_free(url);
    sentry_free(headers);
    return result;
}

sentry_transport_t *
sentry__transport_new_default(void)
{
    SENTRY_INFO("initializing winhttp transport");
    winhttp_client_t *client = winhttp_client_new();
    if (!client) {
        return NULL;
    }

    sentry_transport_t *transport
        = sentry__http_transport_new(client, winhttp_send_task);
    if (!transport) {
        winhttp_client_free(client);
        return NULL;
    }
    sentry__http_transport_set_free_client(transport, winhttp_client_free);
    sentry__http_transport_set_start_client(transport, winhttp_client_start);
    sentry__http_transport_set_shutdown_client(
        transport, winhttp_client_shutdown);
    return transport;
}
