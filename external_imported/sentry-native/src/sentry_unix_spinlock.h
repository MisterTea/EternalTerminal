#ifndef SENTRY_UNIX_SPINLOCK_H_INCLUDED
#define SENTRY_UNIX_SPINLOCK_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_cpu_relax.h"

typedef volatile sig_atomic_t sentry_spinlock_t;

/**
 * On UNIX Systems, inside the signal handler, sentry will switch from standard
 * `malloc` to a custom page-based allocator, which is protected by this special
 * spinlock.
 */

#define SENTRY__SPINLOCK_INIT 0
#define sentry__spinlock_lock(spinlock_ref)                                    \
    for (;;) {                                                                 \
        while (__atomic_load_n(spinlock_ref, __ATOMIC_RELAXED)) {              \
            sentry__cpu_relax();                                               \
        }                                                                      \
        if (__atomic_exchange_n(spinlock_ref, 1, __ATOMIC_ACQUIRE) == 0) {     \
            break;                                                             \
        }                                                                      \
    }
#define sentry__spinlock_unlock(spinlock_ref)                                  \
    (__atomic_store_n(spinlock_ref, 0, __ATOMIC_RELEASE))

#endif
