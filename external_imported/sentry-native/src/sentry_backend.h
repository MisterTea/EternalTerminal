#ifndef SENTRY_BACKEND_H_INCLUDED
#define SENTRY_BACKEND_H_INCLUDED

#include "sentry_boot.h"

#include "sentry_scope.h"

/**
 * This represents the crash handling backend.
 * It consists of a few hooks that integrate into the sentry lifecycle and which
 * can ensure that any captured crash contains the sentry scope and other
 * information.
 */
struct sentry_backend_s;
typedef struct sentry_backend_s {
    int (*startup_func)(
        struct sentry_backend_s *, const sentry_options_t *options);
    void (*shutdown_func)(struct sentry_backend_s *);
    void (*free_func)(struct sentry_backend_s *);
    void (*except_func)(
        struct sentry_backend_s *, const struct sentry_ucontext_s *);
    void (*flush_scope_func)(struct sentry_backend_s *);
    // NOTE: The breadcrumb is not moved into the hook and does not need to be
    // `decref`-d internally.
    void (*add_breadcrumb_func)(
        struct sentry_backend_s *, sentry_value_t breadcrumb);
    void (*user_consent_changed_func)(struct sentry_backend_s *);
    uint64_t (*get_last_crash_func)(struct sentry_backend_s *);
    void *data;
    bool can_capture_after_shutdown;
} sentry_backend_t;

/**
 * This will free a previously allocated backend.
 */
void sentry__backend_free(sentry_backend_t *backend);

/**
 * Create a new backend, depending on build-time configuration.
 */
sentry_backend_t *sentry__backend_new(void);

#endif
