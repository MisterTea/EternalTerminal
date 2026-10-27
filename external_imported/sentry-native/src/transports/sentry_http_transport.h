#ifndef SENTRY_HTTP_TRANSPORT_H_INCLUDED
#define SENTRY_HTTP_TRANSPORT_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_path.h"
#include "sentry_ratelimiter.h"
#include "sentry_sync.h"
#include "sentry_transport.h"

typedef struct sentry_prepared_http_header_s {
    const char *key;
    char *value;
} sentry_prepared_http_header_t;

typedef struct sentry_prepared_http_request_s {
    const char *method;
    char *url;
    sentry_prepared_http_header_t *headers;
    size_t headers_len;
    char *body;
    size_t body_len;
    bool body_owned;
    sentry_path_t *body_path;
} sentry_prepared_http_request_t;

sentry_prepared_http_request_t *sentry__prepare_http_request(
    sentry_envelope_t *envelope, const sentry_dsn_t *dsn,
    const sentry_rate_limiter_t *rl, const char *user_agent);
sentry_prepared_http_request_t *sentry__prepare_tus_create_request(
    size_t file_size, const char *attachment_type, const sentry_dsn_t *dsn,
    const char *user_agent);
sentry_prepared_http_request_t *sentry__prepare_tus_upload_request(
    const char *location, const sentry_path_t *path, size_t file_size,
    const sentry_dsn_t *dsn, const char *user_agent);

void sentry__prepared_http_request_free(sentry_prepared_http_request_t *req);

typedef struct {
    int status_code;
    char *retry_after;
    char *x_sentry_rate_limits;
    char *location;
    bool shutdown;
} sentry_http_response_t;

typedef bool (*sentry_http_send_func_t)(void *client,
    sentry_prepared_http_request_t *req, sentry_http_response_t *resp);

/**
 * Creates a new HTTP transport with the given client and send function.
 * Use the setter functions below to configure optional client callbacks.
 */
sentry_transport_t *sentry__http_transport_new(
    void *client, sentry_http_send_func_t send_func);

void sentry__http_transport_set_free_client(
    sentry_transport_t *transport, void (*free_client)(void *));
void sentry__http_transport_set_start_client(sentry_transport_t *transport,
    int (*start_client)(void *, const sentry_options_t *));
void sentry__http_transport_set_shutdown_client(
    sentry_transport_t *transport, void (*shutdown_client)(void *));

#ifdef SENTRY_UNITTEST
void *sentry__http_transport_get_bgworker(sentry_transport_t *transport);
#endif

#endif
