#include "sentry_alloc.h"
#include "sentry_core.h"
#include "sentry_envelope.h"
#include "sentry_http_transport.h"
#include "sentry_options.h"
#include "sentry_slice.h"
#include "sentry_string.h"
#include "sentry_sync.h"
#include "sentry_transport.h"
#include "sentry_utils.h"

#include <curl/curl.h>
#include <curl/easy.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef SENTRY_PLATFORM_NX
#    include "sentry_transport_curl_nx.h"
#endif

typedef struct {
    CURL *curl_handle;
    char *proxy;
    char *ca_certs;
    bool debug;
    uint64_t transfer_timeout;
    long shutdown;
#ifdef SENTRY_PLATFORM_NX
    void *nx_state;
#endif
} curl_client_t;

typedef struct {
    FILE *file;
    const sentry_path_t *path;
} file_body_t;

static curl_client_t *
curl_client_new(void)
{
    curl_client_t *client = SENTRY_MAKE(curl_client_t);
    if (!client) {
        return NULL;
    }

#ifdef SENTRY_PLATFORM_NX
    client->nx_state = sentry_nx_curl_state_new();
#endif
    return client;
}

static void
curl_client_free(void *_client)
{
    curl_client_t *client = _client;
    if (client->curl_handle) {
        curl_easy_cleanup(client->curl_handle);
        curl_global_cleanup();
    }
    sentry_free(client->ca_certs);
    sentry_free(client->proxy);
#ifdef SENTRY_PLATFORM_NX
    sentry_nx_curl_state_free(client->nx_state);
#endif
    sentry_free(client);
}

static int
curl_client_start(void *_client, const sentry_options_t *options)
{
    curl_client_t *client = _client;

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

    client->proxy = sentry__string_clone(options->proxy);
    client->ca_certs = sentry__string_clone(options->ca_certs);
    client->curl_handle = curl_easy_init();
    client->debug = options->debug;
    client->transfer_timeout = options->transfer_timeout;

    if (!client->curl_handle) {
        // In this case we don't start the worker at all, which means we can
        // still dump all unsent envelopes to disk on shutdown.
        SENTRY_WARN("`curl_easy_init` failed");
        return 1;
    }

#ifdef SENTRY_PLATFORM_NX
    if (!sentry_nx_transport_start(client->nx_state, options)) {
        return 1;
    }
#endif

    return 0;
}

static long
curl_timeout_ms(uint64_t timeout)
{
    return timeout > (uint64_t)LONG_MAX ? LONG_MAX : (long)timeout;
}

static void
curl_client_shutdown(void *_client)
{
    curl_client_t *client = _client;
    sentry__atomic_store(&client->shutdown, 1);
}

static int
progress_callback(void *clientp, curl_off_t UNUSED(dltotal),
    curl_off_t UNUSED(dlnow), curl_off_t UNUSED(ultotal),
    curl_off_t UNUSED(ulnow))
{
    curl_client_t *client = clientp;
    return sentry__atomic_fetch(&client->shutdown) ? 1 : 0;
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
    sentry_http_response_t *info = userdata;
    char *header = sentry__string_clone_n(buffer, bytes);
    if (!header) {
        return bytes;
    }

    char *sep = strchr(header, ':');
    if (sep) {
        *sep = '\0';
        sentry__string_ascii_lower(header);
        sentry_slice_t value
            = sentry__slice_trim(sentry__slice_from_str(sep + 1));
        if (sentry__string_eq(header, "retry-after")) {
            sentry_free(info->retry_after);
            info->retry_after = sentry__slice_to_owned(value);
        } else if (sentry__string_eq(header, "x-sentry-rate-limits")) {
            sentry_free(info->x_sentry_rate_limits);
            info->x_sentry_rate_limits = sentry__slice_to_owned(value);
        } else if (sentry__string_eq(header, "location")) {
            sentry_free(info->location);
            info->location = sentry__slice_to_owned(value);
        }
    }

    sentry_free(header);
    return bytes;
}

static size_t
file_read_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
    file_body_t *body = userdata;
    if (size && nitems > SIZE_MAX / size) {
        goto fail;
    }
    size_t capacity = size * nitems;
    size_t read = fread(buffer, 1, capacity, body->file);
    if (read < capacity && ferror(body->file)) {
        goto fail;
    }
    return read;

fail:
    SENTRY_WARNF("failed to read request body file \"%s\"",
        sentry__path_filename(body->path));
    return CURL_READFUNC_ABORT;
}

static bool
curl_send_task(void *_client, sentry_prepared_http_request_t *req,
    sentry_http_response_t *resp)
{
    curl_client_t *client = (curl_client_t *)_client;

#ifdef SENTRY_PLATFORM_NX
    if (!sentry_nx_curl_connect(client->nx_state)) {
        return false; // TODO should we dump the envelope to disk?
    }
#endif

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

    CURL *curl = client->curl_handle;
    curl_easy_reset(curl);
    if (client->debug) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, stderr);
        // CURLOPT_WRITEFUNCTION will `fwrite` by default
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, swallow_data);
    }
    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, SENTRY_SDK_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(
        curl, CURLOPT_TIMEOUT_MS, curl_timeout_ms(client->transfer_timeout));
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, client);

    FILE *body_file = NULL;
    file_body_t file_body = { 0 };
    if (req->body_path) {
#ifdef SENTRY_PLATFORM_WINDOWS
        body_file = _wfopen(req->body_path->path_w, L"rb");
#else
        body_file = fopen(req->body_path->path, "rb");
#endif
        if (!body_file) {
            SENTRY_WARNF("failed to open request body file \"%s\"",
                sentry__path_filename(req->body_path));
            curl_slist_free_all(headers);
            return false;
        }
        file_body.file = body_file;
        file_body.path = req->body_path;
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req->method);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, file_read_callback);
        curl_easy_setopt(curl, CURLOPT_READDATA, &file_body);
        curl_easy_setopt(
            curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)req->body_len);
    } else if (req->body) {
        curl_easy_setopt(curl, CURLOPT_POST, (long)1);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req->body_len);
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req->method);
    }

    char error_buf[CURL_ERROR_SIZE];
    error_buf[0] = 0;
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buf);

    curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);

    if (client->proxy) {
        curl_easy_setopt(curl, CURLOPT_PROXY, client->proxy);
    }
    if (client->ca_certs) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, client->ca_certs);
    }

#ifdef SENTRY_PLATFORM_NX
    CURLcode rv = sentry_nx_curl_easy_setopt(client->nx_state, curl, req);
#else
    CURLcode rv = CURLE_OK;
#endif

    if (rv == CURLE_OK) {
        rv = curl_easy_perform(curl);
    }

    if (rv == CURLE_OK) {
        long response_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        resp->status_code = (int)response_code;
    } else {
        resp->shutdown = sentry__atomic_fetch(&client->shutdown) != 0;
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

    if (body_file) {
        fclose(body_file);
    }
    curl_slist_free_all(headers);
    return rv == CURLE_OK;
}

sentry_transport_t *
sentry__transport_new_default(void)
{
    SENTRY_INFO("initializing curl transport");
    curl_client_t *client = curl_client_new();
    if (!client) {
        return NULL;
    }

    sentry_transport_t *transport
        = sentry__http_transport_new(client, curl_send_task);
    if (!transport) {
        curl_client_free(client);
        return NULL;
    }
    sentry__http_transport_set_free_client(transport, curl_client_free);
    sentry__http_transport_set_start_client(transport, curl_client_start);
    sentry__http_transport_set_shutdown_client(transport, curl_client_shutdown);
    return transport;
}
