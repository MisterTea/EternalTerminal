#include "sentry_transport.h"
#include "sentry_alloc.h"
#include "sentry_envelope.h"
#include "sentry_options.h"

struct sentry_transport_s {
    void (*send_envelope_func)(sentry_envelope_t *envelope, void *state);
    int (*startup_func)(const sentry_options_t *options, void *state);
    int (*shutdown_func)(uint64_t timeout, void *state);
    int (*flush_func)(uint64_t timeout, void *state);
    void (*free_func)(void *state);
    size_t (*dump_func)(sentry_run_t *run, void *state);
    void (*retry_func)(void *state);
    void (*cleanup_func)(const sentry_options_t *options, void *state);
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
    if (transport && transport->flush_func && transport->running) {
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

void *
sentry__transport_get_state(sentry_transport_t *transport)
{
    return transport ? transport->state : NULL;
}

void
sentry_transport_retry(sentry_transport_t *transport)
{
    if (transport && transport->retry_func) {
        transport->retry_func(transport->state);
    }
}

void
sentry__transport_set_retry_func(
    sentry_transport_t *transport, void (*retry_func)(void *state))
{
    transport->retry_func = retry_func;
}

void
sentry__transport_set_cleanup_func(sentry_transport_t *transport,
    void (*cleanup_func)(const sentry_options_t *options, void *state))
{
    transport->cleanup_func = cleanup_func;
}

bool
sentry__transport_submit_cleanup(
    sentry_transport_t *transport, const sentry_options_t *options)
{
    if (transport && transport->cleanup_func && transport->running) {
        transport->cleanup_func(options, transport->state);
        return true;
    }
    return false;
}
