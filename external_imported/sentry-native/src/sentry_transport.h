#ifndef SENTRY_TRANSPORT_H_INCLUDED
#define SENTRY_TRANSPORT_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_database.h"
#include "sentry_utils.h"

/**
 * Sets the dump function of the transport.
 *
 * This function is called during a hard crash to dump any internal send queue
 * to disk, using `sentry__run_write_envelope`. The function runs inside a
 * signal handler, and appropriate restrictions apply.
 */
void sentry__transport_set_dump_func(sentry_transport_t *transport,
    size_t (*dump_func)(sentry_run_t *run, void *state));

/**
 * Submit the given envelope to the transport.
 */
void sentry__transport_send_envelope(
    sentry_transport_t *transport, sentry_envelope_t *envelope);

/**
 * Calls the transports startup hook.
 *
 * Returns 0 on success.
 */
int sentry__transport_startup(
    sentry_transport_t *transport, const sentry_options_t *options);

/**
 * Instructs the transport to flush its queue.
 *
 * Returns 0 on success.
 */
int sentry__transport_flush(sentry_transport_t *transport, uint64_t timeout);

/**
 * Instructs the transport to shut down.
 *
 * Returns 0 on success.
 */
int sentry__transport_shutdown(sentry_transport_t *transport, uint64_t timeout);

/**
 * This will create a new platform specific HTTP transport.
 */
sentry_transport_t *sentry__transport_new_default(void);

/**
 * This function will instruct the platform specific transport to dump all the
 * envelopes in its send queue to disk.
 */
size_t sentry__transport_dump_queue(
    sentry_transport_t *transport, sentry_run_t *run);

void *sentry__transport_get_state(sentry_transport_t *transport);

void sentry__transport_set_retry_func(
    sentry_transport_t *transport, void (*retry_func)(void *state));

/**
 * Sets the cleanup function of the transport.
 *
 * This function submits cache cleanup as a task on the transport's background
 * worker, so it runs after any pending send tasks from process_old_runs.
 */
void sentry__transport_set_cleanup_func(sentry_transport_t *transport,
    void (*cleanup_func)(const sentry_options_t *options, void *state));

/**
 * Submits cache cleanup to the transport's background worker.
 *
 * Returns true if cleanup was submitted, false if the transport does not
 * support async cleanup (caller should run cleanup synchronously).
 */
bool sentry__transport_submit_cleanup(
    sentry_transport_t *transport, const sentry_options_t *options);

#endif
