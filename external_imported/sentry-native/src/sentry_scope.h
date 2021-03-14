#ifndef SENTRY_SCOPE_H_INCLUDED
#define SENTRY_SCOPE_H_INCLUDED

#include "sentry_boot.h"

#include "sentry_session.h"
#include "sentry_value.h"

/**
 * This represents the current scope.
 */
typedef struct sentry_scope_s {
    char *transaction;
    sentry_value_t fingerprint;
    sentry_value_t user;
    sentry_value_t tags;
    sentry_value_t extra;
    sentry_value_t contexts;
    sentry_value_t breadcrumbs;
    sentry_level_t level;
    sentry_value_t client_sdk;
    sentry_session_t *session;
} sentry_scope_t;

/**
 * When applying a scope to an event object, this specifies all the additional
 * data that should be added to the event.
 */
typedef enum {
    SENTRY_SCOPE_NONE = 0x0,
    // Add all the breadcrumbs from the scope to the event.
    SENTRY_SCOPE_BREADCRUMBS = 0x1,
    // Add the module list to the event.
    SENTRY_SCOPE_MODULES = 0x2,
    // Symbolize all the stacktraces on-device which are found in the event.
    SENTRY_SCOPE_STACKTRACES = 0x4,
    // All of the above.
    SENTRY_SCOPE_ALL = ~0,
} sentry_scope_mode_t;

/**
 * This will acquire a lock on the global scope.
 */
sentry_scope_t *sentry__scope_lock(void);

/**
 * Release the lock on the global scope.
 */
void sentry__scope_unlock(void);

/**
 * This will free all the data attached to the global scope
 */
void sentry__scope_cleanup(void);

/**
 * This will notify any backend of scope changes, and persist session
 * information to disk. This function must be called while holding the scope
 * lock, and it will be unlocked internally.
 */
void sentry__scope_flush_unlock(const sentry_scope_t *scope);

/**
 * This will merge the requested data which is in the given `scope` to the given
 * `event`.
 * See `sentry_scope_mode_t` for the different types of data that can be
 * attached.
 */
void sentry__scope_apply_to_event(const sentry_scope_t *scope,
    sentry_value_t event, sentry_scope_mode_t mode);

/**
 * This will update a sessions `distinct_id`, which is generated out of other
 * scope data.
 */
void sentry__scope_session_sync(sentry_scope_t *scope);

/**
 * These are convenience macros to automatically lock/unlock a scope inside a
 * code block.
 */
#define SENTRY_WITH_SCOPE(Scope)                                               \
    for (const sentry_scope_t *Scope = sentry__scope_lock(); Scope;            \
         sentry__scope_unlock(), Scope = NULL)
#define SENTRY_WITH_SCOPE_MUT(Scope)                                           \
    for (sentry_scope_t *Scope = sentry__scope_lock(); Scope;                  \
         sentry__scope_flush_unlock(Scope), Scope = NULL)
#define SENTRY_WITH_SCOPE_MUT_NO_FLUSH(Scope)                                  \
    for (sentry_scope_t *Scope = sentry__scope_lock(); Scope;                  \
         sentry__scope_unlock(), Scope = NULL)

#endif
