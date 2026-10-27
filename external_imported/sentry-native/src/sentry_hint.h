#ifndef SENTRY_HINT_H_INCLUDED
#define SENTRY_HINT_H_INCLUDED

#include "sentry_boot.h"

/**
 * A sentry Hint used to pass additional data along with an event
 * or feedback when it's being captured.
 */
struct sentry_hint_s {
    sentry_attachment_t *attachments;
};

/**
 * Frees a hint (internal use only).
 */
void sentry__hint_free(sentry_hint_t *hint);

#endif
