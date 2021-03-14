#ifndef SENTRY_TRANSPORTS_DISK_TRANSPORT_H_INCLUDED
#define SENTRY_TRANSPORTS_DISK_TRANSPORT_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_database.h"

/**
 * This creates a new transport that serializes envelopes to disk in the given
 * `run` directory.
 * See `sentry__run_write_envelope`.
 */
sentry_transport_t *sentry_new_disk_transport(const sentry_run_t *run);

#endif
