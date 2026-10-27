#ifndef SENTRY_RINGBUFFER_H_INCLUDED
#define SENTRY_RINGBUFFER_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_value.h"

/**
 * A ringbuffer for storing values with a fixed maximum size.
 *
 * This encapsulates the list of arbitrary sentry_value_t items and wraps it
 * with ring-buffer behavior. This list grows exponentially until it reaches
 * max_size, after which it behaves entirely like a fixed-size ringbuffer.
 *
 * This is currently not thread-safe.
 */
typedef struct sentry_ringbuffer_s {
    sentry_value_t list;
    size_t max_size;
    size_t start_idx;
} sentry_ringbuffer_t;

/**
 * Initialize a new ringbuffer with the given maximum size.
 * Returns a pointer to the ringbuffer on success, NULL on failure.
 */
sentry_ringbuffer_t *sentry__ringbuffer_new(size_t max_size);

/**
 * Free a ringbuffer and decref all its contents.
 */
void sentry__ringbuffer_free(sentry_ringbuffer_t *rb);

/**
 * Append a sentry_value_t to the ringbuffer.
 * If the ringbuffer is full, the oldest value will be overwritten.
 * Returns 0 on success.
 */
int sentry__ringbuffer_append(sentry_ringbuffer_t *rb, sentry_value_t value);

/**
 * Convert the ringbuffer to a regular list in chronological order.
 * Returns a new list containing all values in the ringbuffer.
 */
sentry_value_t sentry__ringbuffer_to_list(const sentry_ringbuffer_t *rb);

/**
 * Update the maximum size of the ringbuffer. This only works during
 * initialization, if there are any values in the list the function exits.
 */
void sentry__ringbuffer_set_max_size(sentry_ringbuffer_t *rb, size_t max_size);

#endif
