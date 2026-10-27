#ifndef SENTRY_BATCHER_H_INCLUDED
#define SENTRY_BATCHER_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_client_report.h"
#include "sentry_database.h"
#include "sentry_envelope.h"
#include "sentry_sync.h"
#include "sentry_transport.h"

#ifdef SENTRY_UNITTEST
#    define SENTRY_BATCHER_QUEUE_LENGTH 5
#else
#    define SENTRY_BATCHER_QUEUE_LENGTH 100
#endif

/**
 * Thread lifecycle states for the batching thread.
 */
typedef enum {
    /** Thread is not running (initial state or after shutdown) */
    SENTRY_BATCHER_THREAD_STOPPED = 0,
    /** Thread is starting up but not yet ready */
    SENTRY_BATCHER_THREAD_STARTING = 1,
    /** Thread is running and processing items */
    SENTRY_BATCHER_THREAD_RUNNING = 2,
} sentry_batcher_thread_state_t;

typedef struct {
    sentry_value_t items[SENTRY_BATCHER_QUEUE_LENGTH];
    long index; // (atomic) index for producer threads to get a unique slot
    long adding; // (atomic) count of in-flight writers on this buffer
    long sealed; // (atomic) 0=writeable, 1=sealed (meaning we drop)
} sentry_batcher_buffer_t;

typedef sentry_envelope_item_t *(*sentry_batch_func_t)(
    sentry_envelope_t *envelope, sentry_value_t items);

typedef struct {
    long refcount; // (atomic) reference count
    sentry_batcher_buffer_t buffers[2]; // double buffer
    long active_idx; // (atomic) index to the active buffer
    long flushing; // (atomic) reentrancy guard to the flusher
    long thread_state; // (atomic) sentry_batcher_thread_state_t
    sentry_waitable_flag_t request_flush; // level-triggered flush flag
    sentry_threadid_t batching_thread; // the batching thread
    sentry_batch_func_t batch_func; // function to add items to envelope
    sentry_data_category_t data_category; // for client report discard tracking
    sentry_dsn_t *dsn;
    sentry_transport_t *transport;
    sentry_run_t *run;
} sentry_batcher_t;

typedef struct {
    sentry_batcher_t *ptr;
    long lock; // (atomic) spinlock
} sentry_batcher_ref_t;

#define SENTRY_BATCHER_REF_INIT { NULL, 0 }

sentry_batcher_t *sentry__batcher_new(
    sentry_batch_func_t batch_func, sentry_data_category_t data_category);

/**
 * Acquires a reference to the batcher behind `ref`, atomically incrementing
 * its refcount under the spinlock. Returns NULL if the ref is empty.
 */
sentry_batcher_t *sentry__batcher_acquire(sentry_batcher_ref_t *ref);

/**
 * Lock-free, signal-safe read of `ref->ptr`. No refcount bump, no lock.
 * Only safe when the caller will never release the returned pointer
 * (i.e. crash-safe flush paths where the process is dying).
 */
sentry_batcher_t *sentry__batcher_peek(sentry_batcher_ref_t *ref);

/**
 * Decrements the batcher's refcount and frees it when it reaches zero.
 */
void sentry__batcher_release(sentry_batcher_t *batcher);

/**
 * Atomically swaps the batcher pointer in `ref`, returning the old one.
 * The caller is responsible for shutting down and releasing the old batcher.
 */
sentry_batcher_t *sentry__batcher_swap(
    sentry_batcher_ref_t *ref, sentry_batcher_t *batcher);

bool sentry__batcher_flush(sentry_batcher_t *batcher, bool crash_safe);
bool sentry__batcher_enqueue(sentry_batcher_t *batcher, sentry_value_t item);
void sentry__batcher_startup(
    sentry_batcher_t *batcher, const sentry_options_t *options);
void sentry__batcher_shutdown(sentry_batcher_t *batcher, uint64_t timeout);
void sentry__batcher_flush_crash_safe(sentry_batcher_t *batcher);
void sentry__batcher_force_flush_begin(sentry_batcher_t *batcher);
void sentry__batcher_force_flush_wait(sentry_batcher_t *batcher);

#ifdef SENTRY_UNITTEST
void sentry__batcher_wait_for_thread_startup(sentry_batcher_t *batcher);
#endif

#endif
