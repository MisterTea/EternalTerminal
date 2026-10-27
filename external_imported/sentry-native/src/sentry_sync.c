#include "sentry_sync.h"
#include "sentry_alloc.h"
#include "sentry_core.h"
#include "sentry_string.h"
#include "sentry_utils.h"
#include <stdio.h>
#include <string.h>

#ifdef SENTRY_PLATFORM_WINDOWS
typedef HRESULT(WINAPI *pSetThreadDescription)(
    HANDLE hThread, PCWSTR lpThreadDescription);
static const DWORD MS_VC_EXCEPTION = 0x406D1388;

#    pragma pack(push, 8)
typedef struct {
    DWORD dwType; // Must be 0x1000.
    LPCSTR szName; // Pointer to name (in user addr space).
    DWORD dwThreadID; // Thread ID (-1=caller thread).
    DWORD dwFlags; // Reserved for future use, must be zero.
} THREADNAME_INFO;
#    pragma pack(pop)

static sentry_threadid_t
thread_get_current_threadid(void)
{
    return GetCurrentThread();
}

static int
thread_setname(sentry_threadid_t thread_id, const char *thread_name)
{
    if (!thread_id || !thread_name) {
        return 0;
    }
    // https://docs.microsoft.com/en-us/visualstudio/debugger/how-to-set-a-thread-name-in-native-code?view=vs-2019

    // approach 1: Windows 10 1607+
    pSetThreadDescription func = (pSetThreadDescription)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
    if (func) {
        wchar_t *thread_name_wstr = sentry__string_to_wstr(thread_name);
        HRESULT result = SUCCEEDED(func(thread_id, thread_name_wstr)) ? 0 : 1;
        sentry_free(thread_name_wstr);
        return SUCCEEDED(result) ? 0 : 1;
    }

    // approach 2: Windows Vista+ and MSVC debugger
#    if _WIN32_WINNT >= 0x0600 && defined(_MSC_VER)
    THREADNAME_INFO threadnameInfo;
    threadnameInfo.dwType = 0x1000;
    threadnameInfo.szName = thread_name;
    threadnameInfo.dwThreadID
        = GetThreadId(thread_id); // only available on Windows Vista+
    threadnameInfo.dwFlags = 0;
#        ifdef __clang__
#            pragma clang diagnostic push
#            pragma clang diagnostic ignored "-Wlanguage-extension-token"
#        endif
#        pragma warning(push)
#        pragma warning(disable : 6320 6322)
    __try {
        RaiseException(MS_VC_EXCEPTION, 0,
            sizeof(threadnameInfo) / sizeof(ULONG_PTR),
            (ULONG_PTR *)&threadnameInfo);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#        pragma warning(pop)
#        ifdef __clang__
#            pragma clang diagnostic pop
#        endif
#    endif

    return 0;
}
#else
static sentry_threadid_t
thread_get_current_threadid(void)
{
    return pthread_self();
}

static int
thread_setname(sentry_threadid_t thread_id, const char *thread_name)
{
    if (!thread_id || !thread_name) {
        return 0;
    }

#    ifdef SENTRY_PLATFORM_DARWIN
    // macOS supports thread naming only for current thread
    if (thread_id != pthread_self()) {
        return 1;
    }
    return pthread_setname_np(thread_name);
#    elif defined(SENTRY_PLATFORM_LINUX) /* and possibly others (like BSDs) */
    return pthread_setname_np(thread_id, thread_name);
#    else
    /* XXX: AIX doesn't have it, but PASE does via ILE APIs. */
    return 0;
#    endif
}
#endif

/**
 * Queue operations, locking and Reference counting:
 *
 * The background worker thread itself is reference counted, one reference held
 * by the "main" thread, and one by the background worker thread itself. The
 * worker thread will drop its own reference on shutdown, and the main thread
 * will drop its reference when the transport owning the background worker is
 * being dropped.
 *
 * Also, each task is reference counted, one reference held by the queue, and
 * one by the background thread for the currently executed task. The refcount
 * will be dropped when the task finished executing, and when the task is
 * removed from the queue (either after being executed, or when the task was
 * concurrently removed from the queue).
 *
 * Each access to the queue itself must be done using the `task_lock`.
 * There are two signals, `submit` *to* the worker, signaling a new task, and
 * `done` *from* the worker signaling that it will close down and can be joined.
 */

/**
 * Overflow-safe addition that clamps to UINT64_MAX instead of wrapping.
 */
static uint64_t
add_saturate(uint64_t a, uint64_t b)
{
    return b <= UINT64_MAX - a ? a + b : UINT64_MAX;
}

struct sentry_bgworker_task_s;
typedef struct sentry_bgworker_task_s {
    struct sentry_bgworker_task_s *next_task;
    long refcount;
    uint64_t execute_after;
    sentry_task_exec_func_t exec_func;
    void (*cleanup_func)(void *task_data);
    void *task_data;
} sentry_bgworker_task_t;

static void
sentry__task_incref(sentry_bgworker_task_t *task)
{
    sentry__atomic_fetch_and_add(&task->refcount, 1);
}

static void
sentry__task_decref(sentry_bgworker_task_t *task)
{
    if (sentry__atomic_fetch_and_add(&task->refcount, -1) == 1) {
        if (task->cleanup_func) {
            task->cleanup_func(task->task_data);
        }
        sentry_free(task);
    }
}

struct sentry_bgworker_s {
    sentry_threadid_t thread_id;
    char *thread_name;
    sentry_cond_t submit_signal;
    sentry_cond_t done_signal;
    sentry_mutex_t task_lock;
    sentry_bgworker_task_t *first_task;
    sentry_bgworker_task_t *last_task;
    sentry_bgworker_task_t *current_task;
    void *state;
    void (*free_state)(void *state);
    long refcount;
    long running;
    long draining;
};

sentry_bgworker_t *
sentry__bgworker_new(void *state, void (*free_state)(void *state))
{
    sentry_bgworker_t *bgw = SENTRY_MAKE(sentry_bgworker_t);
    if (!bgw) {
        if (free_state) {
            free_state(state);
        }
        return NULL;
    }
    sentry__thread_init(&bgw->thread_id);
    sentry__mutex_init(&bgw->task_lock);
    sentry__cond_init(&bgw->submit_signal);
    sentry__cond_init(&bgw->done_signal);
    bgw->state = state;
    bgw->free_state = free_state;
    bgw->refcount = 1;
    return bgw;
}

static void
sentry__bgworker_incref(sentry_bgworker_t *bgw)
{
    sentry__atomic_fetch_and_add(&bgw->refcount, 1);
}

void
sentry__bgworker_decref(sentry_bgworker_t *bgw)
{
    if (!bgw || sentry__atomic_fetch_and_add(&bgw->refcount, -1) != 1) {
        return;
    }

    // no need to lock here, as we do have the only reference
    sentry_bgworker_task_t *task = bgw->first_task;
    while (task) {
        sentry_bgworker_task_t *next_task = task->next_task;
        sentry__task_decref(task);
        task = next_task;
    }
    if (bgw->free_state) {
        bgw->free_state(bgw->state);
    }
    sentry__thread_free(&bgw->thread_id);
    sentry__mutex_free(&bgw->task_lock);
    sentry_free(bgw->thread_name);
    sentry_free(bgw);
}

void *
sentry__bgworker_get_state(sentry_bgworker_t *bgw)
{
    return bgw->state;
}

/**
 * Check if the bgworker is done running and can be shut down.
 * This function does *not* internally lock, and it should only be called when
 * the `task_lock` is held!
 */
static bool
sentry__bgworker_is_done(sentry_bgworker_t *bgw)
{
    if (sentry__atomic_fetch(&bgw->draining)) {
        return true;
    }
    return (!bgw->first_task
               || sentry__monotonic_time() < bgw->first_task->execute_after)
        && !sentry__atomic_fetch(&bgw->running);
}

SENTRY_THREAD_FN
worker_thread(void *data)
{
    sentry_bgworker_t *bgw = data;
    SENTRY_DEBUG("background worker thread started");

    // should be called inside thread itself because of MSVC issues and mac
    // https://randomascii.wordpress.com/2015/10/26/thread-naming-in-windows-time-for-something-better/
    // Additionally, `bgw->thread_id` cannot be used reliably because it is
    // subject to initialization race condition: current thread might be running
    // before `bgw->thread_id` is initialized in the thread that started the
    // background worker.
    if (thread_setname(thread_get_current_threadid(), bgw->thread_name)) {
        SENTRY_WARN("failed to set background worker thread name");
    }

    sentry__mutex_lock(&bgw->task_lock);
    while (true) {
        if (sentry__bgworker_is_done(bgw)) {
            sentry__cond_wake(&bgw->done_signal);
            sentry__mutex_unlock(&bgw->task_lock);
            break;
        }

        sentry_bgworker_task_t *task = bgw->first_task;
        if (!task) {
            // this will implicitly release the lock, and re-acquire on wake
            sentry__cond_wait_timeout(
                &bgw->submit_signal, &bgw->task_lock, 1000);
            continue;
        }

        // wait for a delayed task, wake up to new submissions
        {
            uint64_t now = sentry__monotonic_time();
            if (now < task->execute_after) {
                sentry__cond_wait_timeout(&bgw->submit_signal, &bgw->task_lock,
                    (uint32_t)MIN(task->execute_after - now, UINT32_MAX));
                continue;
            }
        }

        sentry__task_incref(task);
        bgw->current_task = task;
        sentry__mutex_unlock(&bgw->task_lock);

        SENTRY_DEBUG("executing task on worker thread");
        task->exec_func(task->task_data, bgw->state);
        // the task can have a refcount of 2, this `decref` here corresponds
        // to the `incref` above which signifies that the task _is being
        // processed_.
        sentry__task_decref(task);

        // check if the queue has been modified concurrently.
        // if not, we pop it and `decref` again, removing the _is inside
        // list_ refcount.
        sentry__mutex_lock(&bgw->task_lock);
        bgw->current_task = NULL;
        if (bgw->first_task == task) {
            bgw->first_task = task->next_task;
            if (task == bgw->last_task) {
                bgw->last_task = NULL;
            }
            sentry__task_decref(task);
        }
    }
    SENTRY_DEBUG("background worker thread shut down");
    // this decref corresponds to the one done below in `sentry__bgworker_start`
    sentry__bgworker_decref(bgw);
    return 0;
}

int
sentry__bgworker_start(sentry_bgworker_t *bgw)
{
    SENTRY_DEBUG("starting background worker thread");
    sentry__atomic_store(&bgw->running, 1);
    sentry__atomic_store(&bgw->draining, 0);
    // this incref moves the reference into the background thread
    sentry__bgworker_incref(bgw);
    if (sentry__thread_spawn(&bgw->thread_id, &worker_thread, bgw) != 0) {
        sentry__atomic_store(&bgw->running, 0);
        sentry__bgworker_decref(bgw);
        return 1;
    }
    return 0;
}

typedef struct {
    long refcount;
    bool was_flushed;
    sentry_cond_t signal;
    sentry_mutex_t lock;
} sentry_flush_task_t;

static void
sentry__flush_task(void *task_data, void *UNUSED(state))
{
    sentry_flush_task_t *flush_task = (sentry_flush_task_t *)task_data;

    sentry__mutex_lock(&flush_task->lock);
    flush_task->was_flushed = true;
    sentry__cond_wake(&flush_task->signal);
    sentry__mutex_unlock(&flush_task->lock);
}

static void
sentry__flush_task_decref(sentry_flush_task_t *task)
{
    if (sentry__atomic_fetch_and_add(&task->refcount, -1) == 1) {
        sentry__mutex_free(&task->lock);
        sentry_free(task);
    }
}

int
sentry__bgworker_flush(sentry_bgworker_t *bgw, uint64_t timeout)
{
    if (!sentry__atomic_fetch(&bgw->running)) {
        SENTRY_WARN("trying to flush non-running thread");
        return 0;
    }
    SENTRY_DEBUG("flushing background worker thread");

    sentry_flush_task_t *flush_task = SENTRY_MAKE(sentry_flush_task_t);
    if (!flush_task) {
        return 1;
    }
    flush_task->refcount = 2; // this thread + background worker
    flush_task->was_flushed = false;
    sentry__cond_init(&flush_task->signal);
    sentry__mutex_init(&flush_task->lock);

    // place the flush sentinel after the last task due within the timeout;
    // tasks delayed beyond the timeout cannot complete in time anyway
    uint64_t before = sentry__monotonic_time();
    uint64_t deadline = add_saturate(before, timeout);
    uint64_t execute_after = before;
    sentry__mutex_lock(&bgw->task_lock);
    for (sentry_bgworker_task_t *t
        = bgw->current_task ? bgw->current_task->next_task : bgw->first_task;
        t && t->execute_after <= deadline; t = t->next_task) {
        if (t->execute_after > execute_after) {
            execute_after = t->execute_after;
        }
    }
    // NOTE: another thread could submit between unlock and submit_at, making
    // execute_after stale. Flush semantics make this harmless.
    sentry__mutex_unlock(&bgw->task_lock);

    sentry__mutex_lock(&flush_task->lock);

    /* submit the task that triggers our condvar once it runs */
    sentry__bgworker_submit_at(bgw, sentry__flush_task,
        (void (*)(void *))sentry__flush_task_decref, flush_task, execute_after);

    uint64_t started = sentry__monotonic_time();
    bool was_flushed = false;
    while (true) {
        was_flushed = flush_task->was_flushed;

        uint64_t now = sentry__monotonic_time();
        if (was_flushed || (now > started && now - started > timeout)) {
            sentry__mutex_unlock(&flush_task->lock);
            sentry__flush_task_decref(flush_task);

            // return `0` on success
            return !was_flushed;
        }

        // this will implicitly release the lock, and re-acquire on wake
        sentry__cond_wait_timeout(&flush_task->signal, &flush_task->lock, 250);
    }
}

static void
shutdown_task(void *task_data, void *UNUSED(state))
{
    sentry_bgworker_t *bgw = task_data;
    sentry__atomic_store(&bgw->running, 0);
}

int
sentry__bgworker_shutdown_cb(sentry_bgworker_t *bgw, uint64_t timeout,
    void (*on_timeout)(void *), void *on_timeout_data)
{
    if (!sentry__atomic_fetch(&bgw->running)) {
        SENTRY_WARN("trying to shut down non-running thread");
        return 0;
    }
    SENTRY_DEBUG("shutting down background worker thread");

    /* submit a task to shut down the queue */
    sentry__bgworker_submit(bgw, shutdown_task, NULL, bgw);

    uint64_t started = sentry__monotonic_time();
    sentry__mutex_lock(&bgw->task_lock);
    while (true) {
        uint64_t now = sentry__monotonic_time();
        if (now > started && now - started > timeout) {
            if (on_timeout) {
                // fire on_timeout to cancel the ongoing task, and give the
                // worker an extra loop cycle up to 250ms to handle the
                // cancellation
                sentry__mutex_unlock(&bgw->task_lock);
                on_timeout(on_timeout_data);
                sentry__atomic_store(&bgw->draining, 1);
                on_timeout = NULL;
                sentry__mutex_lock(&bgw->task_lock);
                // fall through to !running check below
            } else {
                sentry__atomic_store(&bgw->running, 0);
                sentry__thread_detach(bgw->thread_id);
                sentry__mutex_unlock(&bgw->task_lock);
                SENTRY_WARN("background thread failed to shut down cleanly "
                            "within timeout");
                return 1;
            }
        }

        if (!sentry__atomic_fetch(&bgw->running)) {
            sentry__mutex_unlock(&bgw->task_lock);
            sentry__thread_join(bgw->thread_id);
            return 0;
        }

        // this will implicitly release the lock, and re-acquire on wake
        sentry__cond_wait_timeout(&bgw->done_signal, &bgw->task_lock, 250);
    }
}

int
sentry__bgworker_submit(sentry_bgworker_t *bgw,
    sentry_task_exec_func_t exec_func, void (*cleanup_func)(void *task_data),
    void *task_data)
{
    SENTRY_DEBUG("submitting task to background worker thread");
    return sentry__bgworker_submit_at(
        bgw, exec_func, cleanup_func, task_data, sentry__monotonic_time());
}

int
sentry__bgworker_submit_delayed(sentry_bgworker_t *bgw,
    sentry_task_exec_func_t exec_func, void (*cleanup_func)(void *task_data),
    void *task_data, uint64_t delay_ms)
{
    SENTRY_DEBUGF("submitting %" PRIu64
                  " ms delayed task to background worker thread",
        delay_ms);
    uint64_t execute_after = add_saturate(sentry__monotonic_time(), delay_ms);
    return sentry__bgworker_submit_at(
        bgw, exec_func, cleanup_func, task_data, execute_after);
}

int
sentry__bgworker_submit_at(sentry_bgworker_t *bgw,
    sentry_task_exec_func_t exec_func, void (*cleanup_func)(void *task_data),
    void *task_data, uint64_t execute_after)
{
    sentry_bgworker_task_t *task = SENTRY_MAKE(sentry_bgworker_task_t);
    if (!task) {
        if (cleanup_func) {
            cleanup_func(task_data);
        }
        return 1;
    }
    task->next_task = NULL;
    task->refcount = 1;
    task->execute_after = execute_after;
    task->exec_func = exec_func;
    task->cleanup_func = cleanup_func;
    task->task_data = task_data;

    sentry__mutex_lock(&bgw->task_lock);

    if (!bgw->first_task) {
        // empty queue
        bgw->first_task = task;
        bgw->last_task = task;
    } else if (bgw->last_task->execute_after <= task->execute_after) {
        // append last (common fast path for FIFO immediates)
        bgw->last_task->next_task = task;
        bgw->last_task = task;
    } else {
        // insert sorted by execute_after; skip past current_task which
        // may be executing without the lock held
        sentry_bgworker_task_t *prev = bgw->current_task;
        sentry_bgworker_task_t *cur = prev ? prev->next_task : bgw->first_task;
        while (cur && cur->execute_after <= task->execute_after) {
            prev = cur;
            cur = cur->next_task;
        }

        task->next_task = cur;
        if (prev) {
            prev->next_task = task;
        } else {
            bgw->first_task = task;
        }
        if (!task->next_task) {
            bgw->last_task = task;
        }
    }

    sentry__cond_wake(&bgw->submit_signal);
    sentry__mutex_unlock(&bgw->task_lock);

    return 0;
}

size_t
sentry__bgworker_foreach_matching(sentry_bgworker_t *bgw,
    sentry_task_exec_func_t exec_func,
    bool (*callback)(void *task_data, void *data), void *data)
{
    sentry__mutex_lock(&bgw->task_lock);
    sentry_bgworker_task_t *task = bgw->first_task;
    sentry_bgworker_task_t *prev_task = NULL;
    size_t dropped = 0;

    while (task) {
        bool drop_task = false;
        // only consider tasks matching this exec_func
        if (task->exec_func == exec_func) {
            drop_task = !callback || callback(task->task_data, data);
        }

        sentry_bgworker_task_t *next_task = task->next_task;
        if (drop_task) {
            if (prev_task) {
                prev_task->next_task = next_task;
            } else {
                bgw->first_task = next_task;
            }
            if (bgw->current_task == task) {
                bgw->current_task = NULL;
            } else if (bgw->current_task
                && bgw->current_task->next_task == task) {
                bgw->current_task->next_task = next_task;
            }
            sentry__task_decref(task);
            dropped++;
        } else {
            prev_task = task;
        }

        task = next_task;
    }
    bgw->last_task = prev_task;
    sentry__mutex_unlock(&bgw->task_lock);

    return dropped;
}

void
sentry__bgworker_setname(sentry_bgworker_t *bgw, const char *thread_name)
{
    bgw->thread_name = sentry__string_clone(thread_name);
}

#ifdef SENTRY_UNITTEST
const char *
sentry__bgworker_get_thread_name(sentry_bgworker_t *bgw)
{
    return bgw ? bgw->thread_name : NULL;
}
#endif

#if defined(SENTRY_PLATFORM_UNIX) || defined(SENTRY_PLATFORM_NX)
#    include "sentry_cpu_relax.h"
#    include <unistd.h>

static sentry_threadid_t g_signal_handling_thread = { 0 };
static sig_atomic_t g_in_signal_handler __attribute__((aligned(64))) = 0;

bool
sentry__block_for_signal_handler(void)
{
    for (;;) {
        // if there is no signal handler active, we don't need to block
        // we can spin cheaply, but for the return we must acquire
        if (!__atomic_load_n(&g_in_signal_handler, __ATOMIC_RELAXED)) {
            if (__atomic_load_n(&g_in_signal_handler, __ATOMIC_ACQUIRE) == 0) {
                return true;
            }
        }

        // if we are on the signal handler thread we can also leave
        if (sentry__threadid_equal(sentry__current_thread(),
                __atomic_load_n(&g_signal_handling_thread, __ATOMIC_ACQUIRE))) {
            return false;
        }

        // otherwise, we spin
        sentry__cpu_relax();
    }
}

// Tracks recursive entry depth for the signal handling thread.
// 0 = not in handler, 1 = first entry, 2 = re-entry (skip hooks), 3+ = bail out
static volatile sig_atomic_t g_signal_handler_depth = 0;

int
sentry__enter_signal_handler(void)
{
    for (;;) {
        // entering a signal handler while another runs, should block us
        // unless we are the signal handling thread (recursive crash)
        while (__atomic_load_n(&g_in_signal_handler, __ATOMIC_RELAXED)) {
            if (sentry__threadid_equal(sentry__current_thread(),
                    __atomic_load_n(
                        &g_signal_handling_thread, __ATOMIC_ACQUIRE))) {
                // same thread re-entering via recursive crash
                int depth = __atomic_add_fetch(
                    &g_signal_handler_depth, 1, __ATOMIC_ACQ_REL);
                return depth;
            }
            sentry__cpu_relax();
        }

        // atomically try to take ownership
        if (__atomic_exchange_n(&g_in_signal_handler, 1, __ATOMIC_ACQ_REL)
            == 0) {
            // once we have, publish the handling thread too
            sentry_threadid_t me = sentry__current_thread();
            __atomic_store_n(&g_signal_handling_thread, me, __ATOMIC_RELEASE);
            __atomic_store_n(&g_signal_handler_depth, 1, __ATOMIC_RELEASE);
            return 1; // first entry
        }

        // otherwise we've been raced, spin
    }
}

void
sentry__leave_signal_handler(void)
{
    // reset handling thread
    __atomic_store_n(
        &g_signal_handling_thread, (sentry_threadid_t) { 0 }, __ATOMIC_RELAXED);

    // reset handler flag
    __atomic_store_n(&g_in_signal_handler, 0, __ATOMIC_RELEASE);
}
#endif
