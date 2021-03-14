#include "sentry_alloc.h"
#include "sentry_core.h"
#include "sentry_envelope.h"
#include "sentry_string.h"
#include "sentry_sync.h"

struct transport_state {
    void (*func)(const sentry_envelope_t *envelope, void *data);
    void *data;
};

static void
send_envelope(sentry_envelope_t *envelope, void *_state)
{
    struct transport_state *state = _state;
    state->func(envelope, state->data);
    sentry_envelope_free(envelope);
}

sentry_transport_t *
sentry_new_function_transport(
    void (*func)(const sentry_envelope_t *envelope, void *data), void *data)
{
    SENTRY_DEBUG("initializing function transport");
    struct transport_state *state = SENTRY_MAKE(struct transport_state);
    if (!state) {
        return NULL;
    }
    state->func = func;
    state->data = data;

    sentry_transport_t *transport = sentry_transport_new(send_envelope);
    if (!transport) {
        sentry_free(state);
        return NULL;
    }
    sentry_transport_set_state(transport, state);
    sentry_transport_set_free_func(transport, sentry_free);

    return transport;
}
