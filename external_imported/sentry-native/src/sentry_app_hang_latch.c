// In-process app-hang detection, shared state and helpers. The latch, heartbeat
// API, and capture predicate are the app-thread hot path: app threads write the
// latch via the heartbeat (timestamped with sentry__monotonic_time), the
// watchdog worker in sentry_app_hang_monitor.c reads it.
#include "sentry_app_hang_latch.h"
#include "sentry_sync.h"
#include "sentry_utils.h"

#include <stdint.h>

#if defined(SENTRY_PLATFORM_WINDOWS)
#    include <windows.h>
#elif defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
#    include <sys/syscall.h>
#    include <unistd.h>
#else // SENTRY_PLATFORM_MACOS and other POSIX
#    include <pthread.h>
#endif

bool
sentry__app_hang_should_capture(
    uint64_t hb, uint64_t now, uint64_t timeout_ms, uint64_t last_fired_hb)
{
    if (hb == 0 || timeout_ms == 0) {
        return false;
    }
    if (now < hb || (now - hb) < timeout_ms) {
        return false;
    }
    if (hb == last_fired_hb) {
        return false; // already fired for this freeze
    }
    return true;
}

// The latch is touched by app threads (writers, via the heartbeat) and the
// single watchdog worker (reader). An explicit mutex would put a lock on the
// heartbeat hot path, not ideal.
// The two fields are accessed with 64-bit atomics:
//
//   - last_heartbeat_ms: written on every heartbeat, read by the worker.
//     Needs 64-bit-atomic access so a 32-bit platform can't observe a torn
//     half-updated timestamp.
//   - target_tid: write-once (0 -> first heartbeating tid). The worker only
//     uses it as a stackwalker argument so a relaxed read is sufficient.
//
// Both fields use the sentry__atomic_*_u64 helpers, which provide full 64-bit
// atomic (tear-free) access on every platform. Where the target has native
// 64-bit atomics (AArch64, x86-64, ...) this is lock-free; on ARMv7 the
// compiler lowers it to a libatomic call backed by a lock pool. That is fine
// here: the access is off any signal handler (both writer and worker run in
// normal thread context) and the heartbeat cadence dwarfs the few-ns lock, so
// the lock only delivers the tear-free guarantee we already need.
static uint64_t g_target_tid = 0;
static uint64_t g_last_heartbeat_ms = 0;
static volatile long g_app_hang_active = 0;

void
sentry__app_hang_set_active(bool active)
{
    sentry__atomic_store(&g_app_hang_active, active ? 1 : 0);
}

bool
sentry__app_hang_is_active(void)
{
    return sentry__atomic_fetch(&g_app_hang_active) != 0;
}

uint64_t
sentry__app_hang_current_tid(void)
{
#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    return (uint64_t)syscall(SYS_gettid);
#elif defined(SENTRY_PLATFORM_MACOS)
    uint64_t tid = 0;
    pthread_threadid_np(pthread_self(), &tid);
    return tid;
#elif defined(SENTRY_PLATFORM_WINDOWS)
    return (uint64_t)GetCurrentThreadId();
#else
    return 0;
#endif
}

sentry_app_hang_latch_t
sentry__app_hang_current_latch(void)
{
    sentry_app_hang_latch_t latch;
    latch.target_tid = sentry__atomic_fetch_u64(&g_target_tid);
    latch.last_heartbeat_ms = sentry__atomic_fetch_u64(&g_last_heartbeat_ms);
    return latch;
}

void
sentry__app_hang_latch_reset(void)
{
    sentry__atomic_store_u64(&g_target_tid, 0);
    sentry__atomic_store_u64(&g_last_heartbeat_ms, 0);
}

void
sentry_app_hang_heartbeat(void)
{
    if (!sentry__atomic_fetch(&g_app_hang_active)) {
        return;
    }
    uint64_t tid = sentry__app_hang_current_tid();

    uint64_t target = sentry__atomic_fetch_u64(&g_target_tid);
    if (target == 0) {
        // Latch the first heartbeating thread.
        sentry__atomic_store_u64(&g_target_tid, tid);
        target = tid;
    }
    if (target == tid) {
        // ignore heartbeats from other threads
        sentry__atomic_store_u64(
            &g_last_heartbeat_ms, sentry__monotonic_time());
    }
}
