#include "sentry_transport.h"
#include "sentry_alloc.h"
#include "sentry_envelope.h"
#include "sentry_options.h"
#include "sentry_ratelimiter.h"
#include "sentry_string.h"

#ifdef SENTRY_TRANSPORT_COMPRESSION
#    include "zlib.h"
#endif

#define ENVELOPE_MIME "application/x-sentry-envelope"
#ifdef SENTRY_TRANSPORT_COMPRESSION
// The headers we use are: `x-sentry-auth`, `content-type`, `content-encoding`,
// `content-length`
#    define MAX_HTTP_HEADERS 4
#else
// The headers we use are: `x-sentry-auth`, `content-type`, `content-length`
#    define MAX_HTTP_HEADERS 3
#endif

struct sentry_transport_s {
    void (*send_envelope_func)(sentry_envelope_t *envelope, void *state);
    int (*startup_func)(const sentry_options_t *options, void *state);
    int (*shutdown_func)(uint64_t timeout, void *state);
    int (*flush_func)(uint64_t timeout, void *state);
    void (*free_func)(void *state);
    size_t (*dump_func)(sentry_run_t *run, void *state);
    void *state;
    bool running;
};

sentry_transport_t *
sentry_transport_new(
    void (*send_func)(sentry_envelope_t *envelope, void *state))
{
    sentry_transport_t *transport = SENTRY_MAKE(sentry_transport_t);
    if (!transport) {
        return NULL;
    }
    memset(transport, 0, sizeof(sentry_transport_t));
    transport->send_envelope_func = send_func;

    return transport;
}
void
sentry_transport_set_state(sentry_transport_t *transport, void *state)
{
    transport->state = state;
}
void
sentry_transport_set_free_func(
    sentry_transport_t *transport, void (*free_func)(void *state))
{
    transport->free_func = free_func;
}

void
sentry_transport_set_startup_func(sentry_transport_t *transport,
    int (*startup_func)(const sentry_options_t *options, void *state))
{
    transport->startup_func = startup_func;
}

void
sentry_transport_set_shutdown_func(sentry_transport_t *transport,
    int (*shutdown_func)(uint64_t timeout, void *state))
{
    transport->shutdown_func = shutdown_func;
}

void
sentry_transport_set_flush_func(sentry_transport_t *transport,
    int (*flush_func)(uint64_t timeout, void *state))
{
    transport->flush_func = flush_func;
}

void
sentry__transport_send_envelope(
    sentry_transport_t *transport, sentry_envelope_t *envelope)
{
    if (!envelope) {
        return;
    }
    if (!transport) {
        SENTRY_WARN("discarding envelope due to invalid transport");
        sentry_envelope_free(envelope);
        return;
    }
    SENTRY_DEBUG("sending envelope");
    transport->send_envelope_func(envelope, transport->state);
}

int
sentry__transport_startup(
    sentry_transport_t *transport, const sentry_options_t *options)
{
    if (transport->startup_func) {
        SENTRY_DEBUG("starting transport");
        int rv = transport->startup_func(options, transport->state);
        transport->running = rv == 0;
        return rv;
    }
    return 0;
}

int
sentry__transport_flush(sentry_transport_t *transport, uint64_t timeout)
{
    if (transport->flush_func && transport->running) {
        SENTRY_DEBUG("flushing transport");
        return transport->flush_func(timeout, transport->state);
    }
    return 0;
}

int
sentry__transport_shutdown(sentry_transport_t *transport, uint64_t timeout)
{
    if (transport->shutdown_func && transport->running) {
        SENTRY_DEBUG("shutting down transport");
        transport->running = false;
        return transport->shutdown_func(timeout, transport->state);
    }
    return 0;
}

void
sentry__transport_set_dump_func(sentry_transport_t *transport,
    size_t (*dump_func)(sentry_run_t *run, void *state))
{
    transport->dump_func = dump_func;
}

size_t
sentry__transport_dump_queue(sentry_transport_t *transport, sentry_run_t *run)
{
    if (!transport || !transport->dump_func) {
        return 0;
    }
    size_t dumped = transport->dump_func(run, transport->state);
    if (dumped) {
        SENTRY_DEBUGF("dumped %zu in-flight envelopes to disk", dumped);
    }
    return dumped;
}

void
sentry_transport_free(sentry_transport_t *transport)
{
    if (!transport) {
        return;
    }
    if (transport->free_func) {
        transport->free_func(transport->state);
    }
    sentry_free(transport);
}

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
    sentry_free(req);
}
