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
const DWORD MS_VC_EXCEPTION = 0x406D1388;

#    pragma pack(push, 8)
typedef struct {
    DWORD dwType; // Must be 0x1000.
    LPCSTR szName; // Pointer to name (in user addr space).
    DWORD dwThreadID; // Thread ID (-1=caller thread).
    DWORD dwFlags; // Reserved for future use, must be zero.
} THREADNAME_INFO;
#    pragma pack(pop)

int
sentry__thread_setname(sentry_threadid_t thread_id, const char *thread_name)
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

#        pragma warning(push)
#        pragma warning(disable : 6320 6322)
    __try {
        RaiseException(MS_VC_EXCEPTION, 0,
            sizeof(threadnameInfo) / sizeof(ULONG_PTR),
            (ULONG_PTR *)&threadnameInfo);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#        pragma warning(pop)
#    endif

    return 0;
}
#else
int
sentry__thread_setname(sentry_threadid_t thread_id, const char *thread_name)
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
#    else
    return pthread_setname_np(thread_id, thread_name);
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

struct sentry_bgworker_task_s;
typedef struct sentry_bgworker_task_s {
    struct sentry_bgworker_task_s *next_task;
    long refcount;
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
    void *state;
    void (*free_state)(void *state);
    long refcount;
    long running;
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
    memset(bgw, 0, sizeof(sentry_bgworker_t));
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
    return !bgw->first_task && !sentry__atomic_fetch(&bgw->running);
}

#ifdef _MSC_VER
#    define THREAD_FUNCTION_API __stdcall
#else
#    define THREAD_FUNCTION_API
#endif

#if defined(__MINGW32__) && !defined(__MINGW64__)
#    define UNSIGNED_MINGW unsigned
#else
#    define UNSIGNED_MINGW
#endif

// pthreads use `void *` return types, whereas windows uses `DWORD`
#ifdef SENTRY_PLATFORM_WINDOWS
static UNSIGNED_MINGW DWORD THREAD_FUNCTION_API
#else
static void *
#endif
worker_thread(void *data)
{
    sentry_bgworker_t *bgw = data;
    SENTRY_TRACE("background worker thread started");

    // should be called inside thread itself because of MSVC issues and mac
    // https://randomascii.wordpress.com/2015/10/26/thread-naming-in-windows-time-for-something-better/
    if (sentry__thread_setname(bgw->thread_id, bgw->thread_name)) {
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

        sentry__task_incref(task);
        sentry__mutex_unlock(&bgw->task_lock);

        SENTRY_TRACE("executing task on worker thread");
        task->exec_func(task->task_data, bgw->state);
        // the task can have a refcount of 2, this `decref` here corresponds
        // to the `incref` above which signifies that the task _is being
        // processed_.
        sentry__task_decref(task);

        // check if the queue has been modified concurrently.
        // if not, we pop it and `decref` again, removing the _is inside
        // list_ refcount.
        sentry__mutex_lock(&bgw->task_lock);
        if (bgw->first_task == task) {
            bgw->first_task = task->next_task;
            if (task == bgw->last_task) {
                bgw->last_task = NULL;
            }
            sentry__task_decref(task);
        }
    }
    SENTRY_TRACE("background worker thread shut down");
    // this decref corresponds to the one done below in `sentry__bgworker_start`
    sentry__bgworker_decref(bgw);
    return 0;
}

int
sentry__bgworker_start(sentry_bgworker_t *bgw)
{
    SENTRY_TRACE("starting background worker thread");
    sentry__atomic_fetch_and_add(&bgw->running, 1);
    // this incref moves the reference into the background thread
    sentry__bgworker_incref(bgw);
    if (sentry__thread_spawn(&bgw->thread_id, &worker_thread, bgw) != 0) {
        sentry__atomic_fetch_and_add(&bgw->running, -1);
        sentry__bgworker_decref(bgw);
        return 1;
    }
    return 0;
}

static void
shutdown_task(void *task_data, void *UNUSED(state))
{
    sentry_bgworker_t *bgw = task_data;
    sentry__atomic_fetch_and_add(&bgw->running, -1);
}

int
sentry__bgworker_shutdown(sentry_bgworker_t *bgw, uint64_t timeout)
{
    if (!sentry__atomic_fetch(&bgw->running)) {
        SENTRY_WARN("trying to shut down non-running thread");
        return 0;
    }
    SENTRY_TRACE("shutting down background worker thread");

    /* submit a task to shut down the queue */
    sentry__bgworker_submit(bgw, shutdown_task, NULL, bgw);

    uint64_t started = sentry__monotonic_time();
    sentry__mutex_lock(&bgw->task_lock);
    while (true) {
        if (sentry__bgworker_is_done(bgw)) {
            sentry__mutex_unlock(&bgw->task_lock);
            sentry__thread_join(bgw->thread_id);
            return 0;
        }

        uint64_t now = sentry__monotonic_time();
        if (now > started && now - started > timeout) {
            sentry__mutex_unlock(&bgw->task_lock);
            SENTRY_WARN(
                "background thread failed to shut down cleanly within timeout");
            return 1;
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
    sentry_bgworker_task_t *task = SENTRY_MAKE(sentry_bgworker_task_t);
    if (!task) {
        if (cleanup_func) {
            cleanup_func(task_data);
        }
        return 1;
    }
    task->next_task = NULL;
    task->refcount = 1;
    task->exec_func = exec_func;
    task->cleanup_func = cleanup_func;
    task->task_data = task_data;

    SENTRY_TRACE("submitting task to background worker thread");
    sentry__mutex_lock(&bgw->task_lock);
    if (!bgw->first_task) {
        bgw->first_task = task;
    }
    if (bgw->last_task) {
        bgw->last_task->next_task = task;
    }
    bgw->last_task = task;
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
            drop_task = callback(task->task_data, data);
        }

        sentry_bgworker_task_t *next_task = task->next_task;
        if (drop_task) {
            if (prev_task) {
                prev_task->next_task = next_task;
            } else {
                bgw->first_task = next_task;
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

#ifdef SENTRY_PLATFORM_UNIX
#    include "sentry_unix_spinlock.h"

static sig_atomic_t g_in_signal_handler = 0;
static sentry_threadid_t g_signal_handling_thread = { 0 };

bool
sentry__block_for_signal_handler(void)
{
    while (__sync_fetch_and_add(&g_in_signal_handler, 0)) {
        if (sentry__threadid_equal(
                sentry__current_thread(), g_signal_handling_thread)) {
            return false;
        }
        sentry__cpu_relax();
    }
    return true;
}

void
sentry__enter_signal_handler(void)
{
    sentry__block_for_signal_handler();
    g_signal_handling_thread = sentry__current_thread();
    __sync_fetch_and_or(&g_in_signal_handler, 1);
}

void
sentry__leave_signal_handler(void)
{
    __sync_fetch_and_and(&g_in_signal_handler, 0);
}
#endif
