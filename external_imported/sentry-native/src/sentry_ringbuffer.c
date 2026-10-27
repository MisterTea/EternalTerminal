#include "sentry_ringbuffer.h"
#include "sentry_alloc.h"
#include "sentry_logger.h"

sentry_ringbuffer_t *
sentry__ringbuffer_new(size_t max_size)
{
    sentry_ringbuffer_t *rb = SENTRY_MAKE(sentry_ringbuffer_t);
    if (!rb) {
        return NULL;
    }

    rb->list = sentry_value_new_list();
    rb->max_size = max_size;
    rb->start_idx = 0;

    return rb;
}

void
sentry__ringbuffer_free(sentry_ringbuffer_t *rb)
{
    if (!rb) {
        return;
    }

    sentry_value_decref(rb->list);
    sentry_free(rb);
}

int
sentry__ringbuffer_append(sentry_ringbuffer_t *rb, sentry_value_t value)
{
    if (!rb || rb->max_size == 0) {
        sentry_value_decref(value);
        return -1;
    }

    size_t current_len = sentry_value_get_length(rb->list);

    if (current_len < rb->max_size) {
        return sentry_value_append(rb->list, value);
    } else if (current_len == rb->max_size) {
        sentry_value_set_by_index(rb->list, rb->start_idx, value);
        rb->start_idx = (rb->start_idx + 1) % rb->max_size;

        return 0;
    } else {
        SENTRY_WARNF("Ringbuffer list size (%zu) exceeds max_size (%zu)",
            current_len, rb->max_size);
        sentry_value_decref(value);
        return -1;
    }
}

sentry_value_t
sentry__ringbuffer_to_list(const sentry_ringbuffer_t *rb)
{
    if (!rb) {
        return sentry_value_new_null();
    }

    size_t current_len = sentry_value_get_length(rb->list);

    sentry_value_t result = sentry_value_new_list();

    for (size_t i = 0; i < current_len; i++) {
        size_t idx = (rb->start_idx + i) % current_len;
        sentry_value_t item = sentry_value_get_by_index(rb->list, idx);
        sentry_value_incref(item);
        sentry_value_append(result, item);
    }

    return result;
}

void
sentry__ringbuffer_set_max_size(sentry_ringbuffer_t *rb, size_t max_size)
{
    if (!rb) {
        return;
    }

    size_t current_len = sentry_value_get_length(rb->list);

    // If there are already values in the ringbuffer, don't change anything
    // This function is only meant to be called during initialization
    if (current_len > 0) {
        return;
    }
    rb->max_size = max_size;
}
