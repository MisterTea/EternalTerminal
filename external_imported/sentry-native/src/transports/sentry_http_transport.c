#include "sentry_http_transport.h"
#include "sentry_alloc.h"
#include "sentry_attachment.h"
#include "sentry_client_report.h"
#include "sentry_database.h"
#include "sentry_envelope.h"
#include "sentry_json.h"
#include "sentry_options.h"
#include "sentry_ratelimiter.h"
#include "sentry_retry.h"
#include "sentry_string.h"
#include "sentry_transport.h"
#include "sentry_utils.h"
#include "sentry_value.h"

#ifdef SENTRY_TRANSPORT_COMPRESSION
#    include "zlib.h"
#endif

#include <string.h>

#define ENVELOPE_MIME "application/x-sentry-envelope"
#define TUS_MIME "application/offset+octet-stream"
#define TUS_MAX_HTTP_HEADERS 4
#ifdef SENTRY_TRANSPORT_COMPRESSION
#    define MAX_HTTP_HEADERS 4
#else
#    define MAX_HTTP_HEADERS 3
#endif

typedef struct {
    sentry_dsn_t *dsn;
    char *user_agent;
    sentry_rate_limiter_t *ratelimiter;
    void *client;
    void (*free_client)(void *);
    int (*start_client)(void *, const sentry_options_t *);
    sentry_http_send_func_t send_func;
    void (*shutdown_client)(void *client);
    sentry_retry_t *retry;
    sentry_cache_keep_t cache_keep;
    sentry_run_t *run;
    bool send_client_reports;
} http_transport_state_t;

#ifdef SENTRY_TRANSPORT_COMPRESSION
static bool
gzipped_with_compression(const char *body, const size_t body_len,
    char **compressed_body, size_t *compressed_body_len)
{
    if (!body || body_len == 0) {
        return false;
    }

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.next_in = (unsigned char *)body;
    stream.avail_in = (unsigned int)body_len;

    int err = deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
        MAX_WBITS + 16, 9, Z_DEFAULT_STRATEGY);
    if (err != Z_OK) {
        SENTRY_WARNF("deflateInit2 failed: %d", err);
        return false;
    }

    size_t len = compressBound((unsigned long)body_len);
    char *buffer = sentry_malloc(len);
    if (!buffer) {
        deflateEnd(&stream);
        return false;
    }

    while (err == Z_OK) {
        stream.next_out = (unsigned char *)(buffer + stream.total_out);
        stream.avail_out = (unsigned int)(len - stream.total_out);
        err = deflate(&stream, Z_FINISH);
    }

    if (err != Z_STREAM_END) {
        SENTRY_WARNF("deflate failed: %d", err);
        sentry_free(buffer);
        buffer = NULL;
        deflateEnd(&stream);
        return false;
    }

    *compressed_body_len = stream.total_out;
    *compressed_body = buffer;

    deflateEnd(&stream);
    return true;
}
#endif

sentry_prepared_http_request_t *
sentry__prepare_http_request(sentry_envelope_t *envelope,
    const sentry_dsn_t *dsn, const sentry_rate_limiter_t *rl,
    const char *user_agent)
{
    if (!dsn || !dsn->is_valid) {
        return NULL;
    }

    size_t body_len = 0;
    bool body_owned = true;
    char *body = sentry_envelope_serialize_ratelimited(
        envelope, rl, &body_len, &body_owned);
    if (!body) {
        return NULL;
    }

#ifdef SENTRY_TRANSPORT_COMPRESSION
    bool compressed = false;
    char *compressed_body = NULL;
    size_t compressed_body_len = 0;
    compressed = gzipped_with_compression(
        body, body_len, &compressed_body, &compressed_body_len);
    if (compressed) {
        if (body_owned) {
            sentry_free(body);
            body_owned = false;
        }
        body = compressed_body;
        body_len = compressed_body_len;
        body_owned = true;
    }
#endif

    sentry_prepared_http_request_t *req
        = SENTRY_MAKE(sentry_prepared_http_request_t);
    if (!req) {
        if (body_owned) {
            sentry_free(body);
        }
        return NULL;
    }
    req->headers = sentry_malloc(
        sizeof(sentry_prepared_http_header_t) * MAX_HTTP_HEADERS);
    if (!req->headers) {
        sentry_free(req);
        if (body_owned) {
            sentry_free(body);
        }
        return NULL;
    }
    req->headers_len = 0;

    req->method = "POST";
    req->url = sentry__dsn_get_envelope_url(dsn);
    req->body_path = NULL;

    sentry_prepared_http_header_t *h;
    h = &req->headers[req->headers_len++];
    h->key = "x-sentry-auth";
    h->value = sentry__dsn_get_auth_header(dsn, user_agent);

    h = &req->headers[req->headers_len++];
    h->key = "content-type";
    h->value = sentry__string_clone(ENVELOPE_MIME);

#ifdef SENTRY_TRANSPORT_COMPRESSION
    if (compressed) {
        h = &req->headers[req->headers_len++];
        h->key = "content-encoding";
        h->value = sentry__string_clone("gzip");
    }
#endif

    h = &req->headers[req->headers_len++];
    h->key = "content-length";
    h->value = sentry__int64_to_string((int64_t)body_len);

    req->body = body;
    req->body_len = body_len;
    req->body_owned = body_owned;

    return req;
}

void
sentry__prepared_http_request_free(sentry_prepared_http_request_t *req)
{
    if (!req) {
        return;
    }
    sentry_free(req->url);
    for (size_t i = 0; i < req->headers_len; i++) {
        sentry_free(req->headers[i].value);
    }
    sentry_free(req->headers);
    if (req->body_owned) {
        sentry_free(req->body);
    }
    sentry__path_free(req->body_path);
    sentry_free(req);
}

static char *
tus_build_upload_metadata(const char *attachment_type)
{
    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_sb(NULL);
    sentry__jsonwriter_write_object_start(jw);
    sentry__jsonwriter_write_key(jw, "attachment_type");
    sentry__jsonwriter_write_str(jw, attachment_type);
    sentry__jsonwriter_write_object_end(jw);

    size_t json_len;
    char *json = sentry__jsonwriter_into_string(jw, &json_len);
    if (!json) {
        return NULL;
    }

    char *encoded = sentry__base64_encode(json, json_len);
    sentry_free(json);
    if (!encoded) {
        return NULL;
    }

    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);
    sentry__stringbuilder_append(&sb, "sentry ");
    sentry__stringbuilder_append(&sb, encoded);
    sentry_free(encoded);

    return sentry__stringbuilder_into_string(&sb);
}

static sentry_prepared_http_request_t *
prepare_tus_request_common(size_t upload_size, const char *attachment_type,
    const sentry_dsn_t *dsn, const char *user_agent)
{
    if (!dsn || !dsn->is_valid) {
        return NULL;
    }

    sentry_prepared_http_request_t *req
        = SENTRY_MAKE(sentry_prepared_http_request_t);
    if (!req) {
        return NULL;
    }
    memset(req, 0, sizeof(*req));

    req->headers = sentry_malloc(
        sizeof(sentry_prepared_http_header_t) * TUS_MAX_HTTP_HEADERS);
    if (!req->headers) {
        sentry_free(req);
        return NULL;
    }
    req->headers_len = 0;

    req->method = "POST";
    req->url = sentry__dsn_get_upload_url(dsn);

    sentry_prepared_http_header_t *h;
    h = &req->headers[req->headers_len++];
    h->key = "x-sentry-auth";
    h->value = sentry__dsn_get_auth_header(dsn, user_agent);

    h = &req->headers[req->headers_len++];
    h->key = "tus-resumable";
    h->value = sentry__string_clone("1.0.0");

    h = &req->headers[req->headers_len++];
    h->key = "upload-length";
    h->value = sentry__uint64_to_string((uint64_t)upload_size);

    if (!sentry__string_empty(attachment_type)) {
        h = &req->headers[req->headers_len++];
        h->key = "upload-metadata";
        h->value = tus_build_upload_metadata(attachment_type);
    }

    return req;
}

static sentry_prepared_http_request_t *
prepare_tus_upload_request(const char *location, const sentry_path_t *path,
    size_t file_size, const sentry_dsn_t *dsn, const char *user_agent)
{
    if (!location || !path) {
        return NULL;
    }

    sentry_prepared_http_request_t *req
        = SENTRY_MAKE(sentry_prepared_http_request_t);
    if (!req) {
        return NULL;
    }
    memset(req, 0, sizeof(*req));

    req->headers = sentry_malloc(
        sizeof(sentry_prepared_http_header_t) * TUS_MAX_HTTP_HEADERS);
    if (!req->headers) {
        sentry_free(req);
        return NULL;
    }
    req->headers_len = 0;

    req->method = "PATCH";
    req->url = sentry__string_clone(location);
    req->body_path = sentry__path_clone(path);
    req->body_len = file_size;

    sentry_prepared_http_header_t *h;
    h = &req->headers[req->headers_len++];
    h->key = "x-sentry-auth";
    h->value = sentry__dsn_get_auth_header(dsn, user_agent);

    h = &req->headers[req->headers_len++];
    h->key = "tus-resumable";
    h->value = sentry__string_clone("1.0.0");

    h = &req->headers[req->headers_len++];
    h->key = "content-type";
    h->value = sentry__string_clone(TUS_MIME);

    h = &req->headers[req->headers_len++];
    h->key = "upload-offset";
    h->value = sentry__string_clone("0");

    return req;
}

static void
http_response_cleanup(sentry_http_response_t *resp)
{
    sentry_free(resp->retry_after);
    sentry_free(resp->x_sentry_rate_limits);
    sentry_free(resp->location);
}

enum {
    RESULT_OK = 0,
    RESULT_ERROR = -1,
    RESULT_SHUTDOWN = -2,
};

static int
http_send_request(http_transport_state_t *state,
    sentry_prepared_http_request_t *req, sentry_http_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));
    if (!state->send_func(state->client, req, resp)) {
        int result = resp->shutdown ? RESULT_SHUTDOWN : RESULT_ERROR;
        http_response_cleanup(resp);
        return result;
    }
    return resp->status_code;
}

static bool
http_handle_error(int status_code)
{
    switch (status_code) {
    case 413:
        SENTRY_ERROR("Envelope was discarded due to size limits (HTTP 413 "
                     "Content Too Large).");
        break;
    default:
        break;
    }
    return status_code >= 400 && status_code != 429;
}

static void
http_update_ratelimiter(
    http_transport_state_t *state, sentry_http_response_t *resp)
{
    if (resp->x_sentry_rate_limits) {
        sentry__rate_limiter_update_from_header(
            state->ratelimiter, resp->x_sentry_rate_limits);
    } else if (resp->retry_after) {
        sentry__rate_limiter_update_from_http_retry_after(
            state->ratelimiter, resp->retry_after);
    } else if (resp->status_code == 429) {
        sentry__rate_limiter_update_from_429(state->ratelimiter);
    }
    http_response_cleanup(resp);
}

// Perform a TUS upload for the file at <cache>/<basename> and put the
// resulting remote location URL (caller frees) in `location_out`.
static int
tus_upload_file(http_transport_state_t *state, const sentry_path_t *cache_path,
    const sentry_attachment_ref_t *ref, char **location_out)
{
    if (sentry__string_empty(ref->path)) {
        return RESULT_ERROR;
    }
    *location_out = NULL;
    sentry_path_t *att_file = sentry__path_join_str(cache_path, ref->path);
    size_t file_size = att_file ? sentry__path_get_size(att_file) : 0;
    if (!att_file || file_size == 0) {
        sentry__path_free(att_file);
        return RESULT_ERROR;
    }

    // Step 1: TUS creation (POST, no body)
    sentry_prepared_http_request_t *req = prepare_tus_request_common(
        file_size, ref->attachment_type, state->dsn, state->user_agent);
    if (!req) {
        sentry__path_free(att_file);
        return RESULT_ERROR;
    }

    sentry_http_response_t resp;
    int status_code = http_send_request(state, req, &resp);
    sentry__prepared_http_request_free(req);

    if (status_code < 0) {
        sentry__path_free(att_file);
        return status_code;
    }

    if (resp.status_code != 201 || !resp.location) {
        sentry__path_free(att_file);
        http_response_cleanup(&resp);
        return RESULT_ERROR;
    }

    // Step 2: TUS upload (PATCH with file body). The placeholder needs the
    // raw `Location` value the TUS endpoint returned (relative path); only the
    // PATCH itself needs an absolute URL.
    char *patch_url = sentry__dsn_resolve_url(state->dsn, resp.location);
    char *location = sentry__string_clone(resp.location);
    http_response_cleanup(&resp);
    if (!location) {
        sentry_free(patch_url);
        sentry__path_free(att_file);
        return RESULT_ERROR;
    }
    req = prepare_tus_upload_request(
        patch_url, att_file, file_size, state->dsn, state->user_agent);
    sentry_free(patch_url);
    sentry__path_free(att_file);
    if (!req) {
        sentry_free(location);
        return RESULT_ERROR;
    }

    status_code = http_send_request(state, req, &resp);
    sentry__prepared_http_request_free(req);
    if (status_code < 0) {
        sentry_free(location);
        return status_code;
    }

    int status = resp.status_code;
    http_response_cleanup(&resp);

    if (status != 204) {
        sentry_free(location);
        return RESULT_ERROR;
    }

    *location_out = location;
    return status;
}

// Collect the non-NULL `path` values from every attachment-ref item in the
// envelope, into a newly-allocated sentry_value_t list (caller decref's).
static sentry_value_t
collect_attachment_refs(const sentry_envelope_t *envelope)
{
    sentry_value_t list = sentry_value_new_list();
    size_t count = sentry__envelope_get_item_count(envelope);
    for (size_t i = 0; i < count; i++) {
        const sentry_envelope_item_t *item
            = sentry__envelope_get_item(envelope, i);
        sentry_attachment_ref_t ref;
        if (!sentry__envelope_item_get_attachment_ref(item, &ref)) {
            continue;
        }
        if (!sentry__string_empty(ref.path)) {
            sentry_value_append(list, sentry_value_new_string(ref.path));
        }
        sentry__attachment_ref_cleanup(&ref);
    }
    return list;
}

static bool
materialize_attachment_refs(sentry_envelope_t *envelope)
{
    // Raw envelopes with attachment-refs need materialization so the transport
    // can collect, resolve, and prune those items. Raw envelopes without
    // attachment-refs stay raw so the serializer can stream them as bytes.
    if (!sentry__envelope_has_content_type(
            envelope, SENTRY_ATTACHMENT_REF_MIME)) {
        return true;
    }
    if (sentry__envelope_materialize(envelope)) {
        return true;
    }
    SENTRY_WARN("dropping malformed envelope with attachment-refs");
    return false;
}

static bool
has_attachment_ref_path(const sentry_envelope_t *envelope, const char *path)
{
    size_t count = sentry__envelope_get_item_count(envelope);
    for (size_t i = 0; i < count; i++) {
        const sentry_envelope_item_t *item
            = sentry__envelope_get_item(envelope, i);
        sentry_attachment_ref_t ref;
        if (!sentry__envelope_item_get_attachment_ref(item, &ref)) {
            continue;
        }
        bool found = ref.path && strcmp(ref.path, path) == 0;
        sentry__attachment_ref_cleanup(&ref);
        if (found) {
            return true;
        }
    }
    return false;
}

// Delete cache-sibling files referenced by `paths`. If `keep` is set, preserve
// paths that are still referenced by that envelope.
static void
prune_attachment_refs(const sentry_run_t *run, sentry_value_t paths,
    const sentry_envelope_t *keep)
{
    if (!run || sentry_value_get_type(paths) != SENTRY_VALUE_TYPE_LIST) {
        return;
    }
    size_t count = sentry_value_get_length(paths);
    for (size_t i = 0; i < count; i++) {
        const char *path
            = sentry_value_as_string(sentry_value_get_by_index(paths, i));
        if (sentry__string_empty(path)
            || (keep && has_attachment_ref_path(keep, path))) {
            continue;
        }
        sentry_path_t *p = sentry__path_join_str(run->cache_path, path);
        if (p) {
            sentry__path_remove(p);
            sentry__path_free(p);
        }
    }
}

// Walk attachment-ref items: for each one with `path` and no `location`, try
// TUS upload and set `location`. If TUS is unavailable or fails for a given
// item, drop it and send the event without the large attachment.
static int
resolve_attachment_refs(
    http_transport_state_t *state, sentry_envelope_t *envelope)
{
    if (!state->run || !envelope || sentry__envelope_is_raw(envelope)) {
        return RESULT_OK;
    }
    const sentry_path_t *cache_path = state->run->cache_path;

    size_t i = 0;
    while (i < sentry__envelope_get_item_count(envelope)) {
        sentry_envelope_item_t *item = sentry__envelope_get_item(envelope, i);
        sentry_attachment_ref_t ref;
        if (!sentry__envelope_item_get_attachment_ref(item, &ref)) {
            i++;
            continue;
        }

        if (sentry__rate_limiter_is_disabled(
                state->ratelimiter, SENTRY_RL_CATEGORY_ERROR)) {
            sentry__attachment_ref_cleanup(&ref);
            sentry__client_report_discard(
                SENTRY_DISCARD_REASON_RATELIMIT_BACKOFF,
                SENTRY_DATA_CATEGORY_ATTACHMENT, 1);
            if (!sentry__envelope_remove_item(envelope, item)) {
                return RESULT_ERROR;
            }
            continue;
        }

        if (!sentry__string_empty(ref.location)) {
            // Crash-resume: TUS already done on a prior attempt. Nothing to do.
            sentry__attachment_ref_cleanup(&ref);
            i++;
            continue;
        }
        if (sentry__string_empty(ref.path)) {
            sentry__attachment_ref_cleanup(&ref);
            if (!sentry__envelope_remove_item(envelope, item)) {
                return RESULT_ERROR;
            }
            continue;
        }

        char *new_location = NULL;
        int result = tus_upload_file(state, cache_path, &ref, &new_location);
        if (new_location) {
            bool resolved = sentry__envelope_item_resolve_attachment_ref(
                item, new_location);
            sentry_free(new_location);
            if (resolved) {
                sentry__attachment_ref_cleanup(&ref);
                i++;
                continue;
            }
            sentry__attachment_ref_cleanup(&ref);
            return RESULT_ERROR;
        }

        if (result == RESULT_SHUTDOWN) {
            sentry__attachment_ref_cleanup(&ref);
            return result;
        }
        sentry__attachment_ref_cleanup(&ref);
        sentry__client_report_discard(SENTRY_DISCARD_REASON_SEND_ERROR,
            SENTRY_DATA_CATEGORY_ATTACHMENT, 1);
        if (!sentry__envelope_remove_item(envelope, item)) {
            return RESULT_ERROR;
        }
    }
    return RESULT_OK;
}

static bool
add_client_report(sentry_envelope_t *envelope, http_transport_state_t *state,
    sentry_client_report_t *report)
{
    if (!state->send_client_reports
        || !sentry__envelope_can_add_client_report(envelope, state->ratelimiter)
        || !sentry__client_report_save(report)) {
        return false;
    }
    if (sentry__envelope_add_client_report(envelope, report)) {
        return true;
    }
    sentry__client_report_restore(report);
    return false;
}

static int
http_send_envelope(sentry_envelope_t *envelope, void *_state)
{
    http_transport_state_t *state = _state;
    int result = resolve_attachment_refs(state, envelope);
    if (result < 0) {
        if (result != RESULT_SHUTDOWN) {
            SENTRY_WARN("failed to resolve attachment-ref items");
        }
        return result;
    }

    sentry_prepared_http_request_t *req = sentry__prepare_http_request(
        envelope, state->dsn, state->ratelimiter, state->user_agent);
    if (!req) {
        return RESULT_OK;
    }
    sentry_http_response_t resp;
    int status_code = http_send_request(state, req, &resp);
    sentry__prepared_http_request_free(req);
    if (status_code >= 0) {
        http_update_ratelimiter(state, &resp);
    }
    return status_code;
}

static int
retry_send_cb(sentry_envelope_t *envelope, void *_state)
{
    http_transport_state_t *state = _state;
    if (!materialize_attachment_refs(envelope)) {
        return 400;
    }
    sentry_client_report_t report = { 0 };
    bool reported = add_client_report(envelope, state, &report);
    sentry_value_t ref_paths = collect_attachment_refs(envelope);
    int status_code = http_send_envelope(envelope, state);
    if (status_code < 0 || state->cache_keep == SENTRY_CACHE_KEEP_ALWAYS) {
        prune_attachment_refs(state->run, ref_paths, envelope);
    } else {
        prune_attachment_refs(state->run, ref_paths, NULL);
    }
    if (status_code <= 0 && reported) {
        sentry__client_report_restore(&report);
    } else if (http_handle_error(status_code) && state->send_client_reports) {
        sentry__client_report_restore(&report);
        if (sentry__envelope_materialize(envelope)) {
            sentry__envelope_discard(
                envelope, SENTRY_DISCARD_REASON_SEND_ERROR, state->ratelimiter);
        }
    }
    sentry_value_decref(ref_paths);
    return status_code;
}

static void
http_transport_state_free(void *_state)
{
    http_transport_state_t *state = _state;
    if (state->free_client) {
        state->free_client(state->client);
    }
    sentry__dsn_decref(state->dsn);
    sentry_free(state->user_agent);
    sentry__rate_limiter_free(state->ratelimiter);
    sentry__retry_free(state->retry);
    sentry__run_free(state->run);
    sentry_free(state);
}

static void
http_send_task(void *_envelope, void *_state)
{
    sentry_envelope_t *envelope = _envelope;
    http_transport_state_t *state = _state;

    sentry_uuid_t event_id = sentry__envelope_get_event_id(envelope);
    if (!materialize_attachment_refs(envelope)) {
        sentry__cache_remove_siblings(state->run, &event_id);
        return;
    }

    sentry_client_report_t report = { 0 };
    bool reported = add_client_report(envelope, state, &report);

    // Capture cached sibling paths before resolving attachment-refs. Dropped
    // attachment-refs no longer carry local paths afterwards.
    sentry_value_t ref_paths = collect_attachment_refs(envelope);
    int status_code = http_send_envelope(envelope, state);

    const sentry_envelope_t *ref_owner = NULL;
    if (status_code < 0) {
        if (sentry_value_get_length(ref_paths) > 0
            && status_code == RESULT_SHUTDOWN
            && sentry__run_write_envelope(state->run, envelope)) {
            // Keep attachment-ref cache siblings for the persisted envelope.
            ref_owner = envelope;
        } else if (state->retry
            && sentry__retry_enqueue(state->retry, envelope)) {
            // Keep attachment-ref cache siblings for the queued retry envelope.
            ref_owner = envelope;
        } else {
            if (!state->retry && state->cache_keep
                && sentry__run_write_cache(state->run, envelope, -1)) {
                // Keep attachment-ref cache siblings for the cached envelope.
                ref_owner = envelope;
            }
            sentry__client_report_restore(&report);
            sentry__envelope_discard(envelope,
                SENTRY_DISCARD_REASON_NETWORK_ERROR, state->ratelimiter);
        }
        prune_attachment_refs(state->run, ref_paths, ref_owner);
    } else {
        if (state->cache_keep == SENTRY_CACHE_KEEP_ALWAYS
            && sentry__run_write_cache(state->run, envelope, -1)) {
            ref_owner = envelope;
        }
        prune_attachment_refs(state->run, ref_paths, ref_owner);
    }
    sentry_value_decref(ref_paths);

    if (status_code == 0 && reported) {
        sentry__client_report_restore(&report);
    } else if (status_code >= 0 && http_handle_error(status_code)) {
        sentry__client_report_restore(&report);
        sentry__envelope_discard(
            envelope, SENTRY_DISCARD_REASON_SEND_ERROR, state->ratelimiter);
    }
}

static void
http_cleanup_cache_task(void *task_data, void *_state)
{
    (void)_state;
    sentry_options_t *options = task_data;
    sentry__cleanup_cache(options);
}

static void
http_transport_shutdown_timeout(void *_state)
{
    http_transport_state_t *state = _state;
    if (state->shutdown_client) {
        state->shutdown_client(state->client);
    }
}

static int
http_transport_start(const sentry_options_t *options, void *transport_state)
{
    sentry_bgworker_t *bgworker = transport_state;
    http_transport_state_t *state = sentry__bgworker_get_state(bgworker);

    sentry__bgworker_setname(bgworker, options->transport_thread_name);

    state->dsn = sentry__dsn_incref(options->dsn);
    state->user_agent = sentry__string_clone(options->user_agent);
    state->cache_keep = options->cache_keep;
    state->run = sentry__run_incref(options->run);
    state->send_client_reports = options->send_client_reports;

    if (state->start_client) {
        int rv = state->start_client(state->client, options);
        if (rv != 0) {
            return rv;
        }
    }

    int rv = sentry__bgworker_start(bgworker);
    if (rv != 0) {
        return rv;
    }

    if (options->http_retry) {
        state->retry = sentry__retry_new(options);
        if (state->retry) {
            sentry__retry_start(state->retry, bgworker, retry_send_cb, state);
        }
    }

    return 0;
}

static int
http_transport_flush(uint64_t timeout, void *transport_state)
{
    sentry_bgworker_t *bgworker = transport_state;
    return sentry__bgworker_flush(bgworker, timeout);
}

static bool
http_flush_cleanup_cb(void *task_data, void *UNUSED(data))
{
    http_cleanup_cache_task(task_data, NULL);
    return true;
}

static int
http_transport_shutdown(uint64_t timeout, void *transport_state)
{
    sentry_bgworker_t *bgworker = transport_state;
    http_transport_state_t *state = sentry__bgworker_get_state(bgworker);

    sentry__retry_shutdown(state->retry);

    int rv = sentry__bgworker_shutdown_cb(
        bgworker, timeout, http_transport_shutdown_timeout, state);
    if (rv != 0) {
        sentry__bgworker_foreach_matching(
            bgworker, http_cleanup_cache_task, http_flush_cleanup_cb, NULL);
        sentry__retry_seal(state->retry);
    }
    return rv;
}

static void
http_transport_send_envelope(sentry_envelope_t *envelope, void *transport_state)
{
    sentry_bgworker_t *bgworker = transport_state;
    sentry__bgworker_submit(bgworker, http_send_task,
        (void (*)(void *))sentry_envelope_free, envelope);
}

static bool
http_dump_task_cb(void *envelope, void *run)
{
    sentry__run_write_envelope(
        (sentry_run_t *)run, (sentry_envelope_t *)envelope);
    return true;
}

static size_t
http_dump_queue(sentry_run_t *run, void *transport_state)
{
    sentry_bgworker_t *bgworker = transport_state;
    return sentry__bgworker_foreach_matching(
        bgworker, http_send_task, http_dump_task_cb, run);
}

static http_transport_state_t *
http_transport_get_state(sentry_transport_t *transport)
{
    sentry_bgworker_t *bgworker = sentry__transport_get_state(transport);
    return sentry__bgworker_get_state(bgworker);
}

static void
http_transport_retry(void *transport_state)
{
    sentry_bgworker_t *bgworker = transport_state;
    http_transport_state_t *state = sentry__bgworker_get_state(bgworker);
    if (state->retry) {
        sentry__retry_trigger(state->retry);
    }
}

static void
http_transport_submit_cleanup(
    const sentry_options_t *options, void *transport_state)
{
    sentry_bgworker_t *bgworker = transport_state;
    sentry__bgworker_submit(bgworker, http_cleanup_cache_task,
        (void (*)(void *))sentry_options_free,
        sentry__options_incref((sentry_options_t *)options));
}

sentry_transport_t *
sentry__http_transport_new(void *client, sentry_http_send_func_t send_func)
{
    http_transport_state_t *state = SENTRY_MAKE(http_transport_state_t);
    if (!state) {
        return NULL;
    }
    state->ratelimiter = sentry__rate_limiter_new();
    state->client = client;
    state->send_func = send_func;

    sentry_bgworker_t *bgworker
        = sentry__bgworker_new(state, http_transport_state_free);
    if (!bgworker) {
        http_transport_state_free(state);
        return NULL;
    }

    sentry_transport_t *transport
        = sentry_transport_new(http_transport_send_envelope);
    if (!transport) {
        sentry__bgworker_decref(bgworker);
        return NULL;
    }

    sentry_transport_set_state(transport, bgworker);
    sentry_transport_set_free_func(
        transport, (void (*)(void *))sentry__bgworker_decref);
    sentry_transport_set_startup_func(transport, http_transport_start);
    sentry_transport_set_flush_func(transport, http_transport_flush);
    sentry_transport_set_shutdown_func(transport, http_transport_shutdown);
    sentry__transport_set_dump_func(transport, http_dump_queue);
    sentry__transport_set_retry_func(transport, http_transport_retry);
    sentry__transport_set_cleanup_func(
        transport, http_transport_submit_cleanup);

    return transport;
}

void
sentry__http_transport_set_free_client(
    sentry_transport_t *transport, void (*free_client)(void *))
{
    http_transport_get_state(transport)->free_client = free_client;
}

void
sentry__http_transport_set_start_client(sentry_transport_t *transport,
    int (*start_client)(void *, const sentry_options_t *))
{
    http_transport_get_state(transport)->start_client = start_client;
}

void
sentry__http_transport_set_shutdown_client(
    sentry_transport_t *transport, void (*shutdown_client)(void *))
{
    http_transport_get_state(transport)->shutdown_client = shutdown_client;
}

#ifdef SENTRY_UNITTEST
void *
sentry__http_transport_get_bgworker(sentry_transport_t *transport)
{
    return sentry__transport_get_state(transport);
}
#endif

sentry_prepared_http_request_t *
sentry__prepare_tus_create_request(size_t file_size,
    const char *attachment_type, const sentry_dsn_t *dsn,
    const char *user_agent)
{
    return prepare_tus_request_common(
        file_size, attachment_type, dsn, user_agent);
}

sentry_prepared_http_request_t *
sentry__prepare_tus_upload_request(const char *location,
    const sentry_path_t *path, size_t file_size, const sentry_dsn_t *dsn,
    const char *user_agent)
{
    return prepare_tus_upload_request(
        location, path, file_size, dsn, user_agent);
}
