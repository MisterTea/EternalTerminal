#ifndef SENTRY_UNIX_SPINLOCK_H_INCLUDED
#define SENTRY_UNIX_SPINLOCK_H_INCLUDED

#include "sentry_boot.h"

typedef volatile sig_atomic_t sentry_spinlock_t;

/**
 * On UNIX Systems, inside the signal handler, sentry will switch from standard
 * `malloc` to a custom page-based allocator, which is protected by this special
 * spinlock.
 */

#if (defined(__i386__) || defined(__amd64__))
#    define sentry__cpu_relax() __asm__ __volatile__("pause\n")
#else
#    define sentry__cpu_relax() (void)0
#endif

#define SENTRY__SPINLOCK_INIT 0
#define sentry__spinlock_lock(spinlock_ref)                                    \
    while (!__sync_bool_compare_and_swap(spinlock_ref, 0, 1)) {                \
        sentry__cpu_relax();                                                   \
    }
#define sentry__spinlock_unlock(spinlock_ref) (*spinlock_ref = 0)

#endif
