#include "sentry_thread_stackwalk.h"

#include "sentry_boot.h"

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)

#    include "sentry_app_hang_latch.h" // SENTRY_APP_HANG_MAX_FRAMES
#    include "sentry_logger.h"

#    include <errno.h>
#    include <semaphore.h>
#    include <signal.h>
#    include <string.h>
#    include <sys/syscall.h>
#    include <time.h>
#    include <unistd.h>

#    if defined(SENTRY_WITH_UNWINDER_LIBUNWIND)
#        define UNW_LOCAL_ONLY
#        include <libunwind.h>
#    endif

#    if defined(SENTRY_WITH_UNWINDER_LIBUNWINDSTACK)
#        include "sentry.h"
#    endif

#    define SENTRY_APP_HANG_SIGNAL (SIGRTMIN + 4)

static sem_t g_done;
static volatile sig_atomic_t g_active = 0;
static void *g_ips[SENTRY_APP_HANG_MAX_FRAMES];
static volatile sig_atomic_t g_count = 0;
static volatile sig_atomic_t g_want = 0;

#    if defined(SENTRY_WITH_UNWINDER_LIBUNWINDSTACK)
static sem_t g_uctx_ready; // handler -> watchdog: parked, uctx valid
static sem_t
    g_unwind_done; // watchdog -> handler: unwind complete, you may return
static volatile sig_atomic_t g_abort_park = 0;
static ucontext_t *volatile g_park_uctx = NULL;
#    endif

static void
handler(int sig, siginfo_t *info, void *ucontext)
{
    (void)sig;
    (void)info;
    // The handler interrupts arbitrary code on the target thread; preserve its
    // errno so the calls below (sem_*, unw_*) don't leak a value back to it.
    const int saved_errno = errno;
    if (!__atomic_load_n(&g_active, __ATOMIC_ACQUIRE)) {
        errno = saved_errno;
        return; // stray/late delivery; ignore
    }
    size_t n = 0;
#    if defined(SENTRY_WITH_UNWINDER_LIBUNWIND)
    // This duplicates the unwind loop in sentry__unwind_stack_libunwind rather
    // than calling it, because we are inside a signal handler on the target
    // thread: the shared unwinder is not async-signal-safe (it calls
    // SENTRY_WARN and open("/proc/self/maps") for SP validation). libunwind has
    // no API to local-unwind another thread off-thread, so we must walk in the
    // handler with this trimmed, signal-safe loop. The libunwindstack path
    // below sidesteps this by parking the handler and unwinding on the watchdog
    // thread, which is why it can reuse sentry_unwind_stack_from_ucontext.
    unw_cursor_t cursor;
    if (unw_init_local2(
            &cursor, (unw_context_t *)ucontext, UNW_INIT_SIGNAL_FRAME)
        == 0) {
        while (n < (size_t)__atomic_load_n(&g_want, __ATOMIC_RELAXED)) {
            unw_word_t ip = 0;
            if (unw_get_reg(&cursor, UNW_REG_IP, &ip) < 0 || ip == 0) {
                break;
            }
            g_ips[n++] = (void *)(uintptr_t)ip;
            if (unw_step(&cursor) <= 0) {
                break;
            }
        }
    }
#    elif defined(SENTRY_WITH_UNWINDER_LIBUNWINDSTACK)
    g_park_uctx = (ucontext_t *)ucontext;
    sem_post(&g_uctx_ready);
    for (;;) {
        if (sem_wait(&g_unwind_done) == 0) {
            break;
        }
        if (errno != EINTR || g_abort_park) {
            break;
        }
    }
    g_park_uctx = NULL;
    // watchdog wrote g_ips/g_count before releasing us
    n = (size_t)__atomic_load_n(&g_count, __ATOMIC_RELAXED);
#    else
    (void)ucontext;
#    endif
    __atomic_store_n(&g_count, (sig_atomic_t)n, __ATOMIC_RELAXED);
    __atomic_store_n(&g_active, 0, __ATOMIC_RELEASE);
    sem_post(&g_done);
    errno = saved_errno;
}

static bool g_installed = false;
static bool g_sem_initialized = false;

static bool
ensure_installed(void)
{
    if (g_installed) {
        return true;
    }
    if (!g_sem_initialized) {
        // Semaphore is process-lifetime; intentionally never sem_destroy'd.
        if (sem_init(&g_done, 0, 0) != 0) {
            SENTRY_DEBUG("app-hang: sem_init failed");
            return false;
        }
#    if defined(SENTRY_WITH_UNWINDER_LIBUNWINDSTACK)
        if (sem_init(&g_uctx_ready, 0, 0) != 0) {
            SENTRY_DEBUG("app-hang: sem_init(g_uctx_ready) failed");
            // Tear down the semaphores already initialized so a later retry
            // starts from a clean slate instead of re-initializing g_done
            // (re-init of a live semaphore is undefined behavior).
            sem_destroy(&g_done);
            return false;
        }
        if (sem_init(&g_unwind_done, 0, 0) != 0) {
            SENTRY_DEBUG("app-hang: sem_init(g_unwind_done) failed");
            sem_destroy(&g_done);
            sem_destroy(&g_uctx_ready);
            return false;
        }
#    endif
        g_sem_initialized = true;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SENTRY_APP_HANG_SIGNAL, &sa, NULL) != 0) {
        SENTRY_DEBUG("app-hang: sigaction failed");
        return false;
    }
#    if defined(SENTRY_WITH_UNWINDER_LIBUNWIND)
    // Prime the unwinder cache so the in-handler unwind never triggers
    // dl_iterate_phdr for the first time inside the signal handler.
    unw_context_t uc;
    unw_cursor_t cur;
#        ifdef __clang__
// This pragma is required to build with Werror on ARM64 Ubuntu
#            pragma clang diagnostic push
#            pragma clang diagnostic ignored                                   \
                "-Wgnu-statement-expression-from-macro-expansion"
#        endif
    int got_context = unw_getcontext(&uc);
#        ifdef __clang__
#            pragma clang diagnostic pop
#        endif
    if (got_context == 0 && unw_init_local(&cur, &uc) == 0) {
        for (int i = 0; i < 5 && unw_step(&cur) > 0; i++) { }
    }
#    endif
    g_installed = true;
    return true;
}

size_t
sentry__thread_stackwalk(uint64_t target_tid, void **ips, size_t max)
{
    if (!ensure_installed()) {
        return 0;
    }
    // A signal from a previously timed-out sample may still be queued. Drain
    // any stale post here. If a late handler runs during THIS sample it simply
    // captures the current (correct) thread state; the worst case is one
    // wasted timeout on the following cycle. Bounded and benign.
    while (sem_trywait(&g_done) == 0) { }
#    if defined(SENTRY_WITH_UNWINDER_LIBUNWINDSTACK)
    // Drain stale rendezvous tokens left by a previously timed-out cycle, and
    // take ownership of the abort flag here (not in the handler) so a late
    // handler cannot clear an abort we are about to set.
    while (sem_trywait(&g_uctx_ready) == 0) { }
    while (sem_trywait(&g_unwind_done) == 0) { }
    g_abort_park = 0;
#    endif

    __atomic_store_n(&g_want,
        (sig_atomic_t)(max < SENTRY_APP_HANG_MAX_FRAMES
                ? max
                : SENTRY_APP_HANG_MAX_FRAMES),
        __ATOMIC_RELAXED);
    __atomic_store_n(&g_count, 0, __ATOMIC_RELAXED);
    // Release: publishes the g_want/g_count writes above to the handler, which
    // observes them via the acquire-load of g_active on signal entry.
    __atomic_store_n(&g_active, 1, __ATOMIC_RELEASE);

    if (syscall(SYS_tgkill, getpid(), (pid_t)target_tid, SENTRY_APP_HANG_SIGNAL)
        != 0) {
        SENTRY_DEBUGF("app-hang: tgkill(%d) failed: %s", (int)target_tid,
            strerror(errno));
        __atomic_store_n(&g_active, 0, __ATOMIC_RELEASE);
        return 0;
    }

#    if defined(SENTRY_WITH_UNWINDER_LIBUNWINDSTACK)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    if (sem_timedwait(&g_uctx_ready, &ts) != 0) {
        g_abort_park = 1;
        sem_post(&g_unwind_done); // release a handler that parks late
        __atomic_store_n(&g_active, 0, __ATOMIC_RELEASE);
        return 0;
    }
    sentry_ucontext_t s;
    memset(&s, 0, sizeof(s));
    s.user_context = g_park_uctx;
    size_t n = sentry_unwind_stack_from_ucontext(&s, ips, max);
    __atomic_store_n(&g_count, (sig_atomic_t)n, __ATOMIC_RELAXED);
    sem_post(&g_unwind_done); // release the parked handler
    struct timespec ts2;
    clock_gettime(CLOCK_REALTIME, &ts2);
    ts2.tv_sec += 1;
    while (sem_timedwait(&g_done, &ts2) != 0 && errno == EINTR) { }
    __atomic_store_n(&g_active, 0, __ATOMIC_RELEASE);
    return n; // ips already filled by sentry_unwind_stack_from_ucontext
#    else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1; // 1s budget for the handler to run
    while (sem_timedwait(&g_done, &ts) != 0) {
        if (errno == EINTR) {
            continue;
        }
        // timed out (e.g. thread in uninterruptible sleep)
        __atomic_store_n(&g_active, 0, __ATOMIC_RELEASE);
        return 0;
    }

    size_t n = (size_t)__atomic_load_n(&g_count, __ATOMIC_RELAXED);
    for (size_t i = 0; i < n && i < max; i++) {
        ips[i] = g_ips[i];
    }
    return n;
#    endif
}

#endif
