#ifndef SENTRY_SYNC_H_INCLUDED
#define SENTRY_SYNC_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_core.h"

#include <assert.h>
#include <stdio.h>

// define a recursive mutex for all platforms
#ifdef SENTRY_PLATFORM_WINDOWS
#    if _WIN32_WINNT >= 0x0600
#        include <synchapi.h>
#    endif
#    include <winnt.h>

#    if _WIN32_WINNT < 0x0600

#        define INIT_ONCE_STATIC_INIT                                          \
            {                                                                  \
                0                                                              \
            }

typedef union {
    PVOID Ptr;
} INIT_ONCE, *PINIT_ONCE;

typedef struct {
    HANDLE Semaphore;
    HANDLE ContinueEvent;
    LONG Waiters;
    LONG Target;
} CONDITION_VARIABLE_PREVISTA, *PCONDITION_VARIABLE_PREVISTA;

typedef BOOL(WINAPI *PINIT_ONCE_FN)(
    PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context);

inline BOOL
InitOnceExecuteOnce(
    PINIT_ONCE InitOnce, PINIT_ONCE_FN InitFn, PVOID Parameter, LPVOID *Context)
{
    for (;;) {
        switch ((ULONG_PTR)InitOnce->Ptr) {
        case 0: // not started
            if (InterlockedCompareExchangePointer(
                    &InitOnce->Ptr, (PVOID)1, (PVOID)0)
                != 0) {
                break;
            }
            if (InitFn(InitOnce, Parameter, Context)) {
                InitOnce->Ptr = (PVOID)2;
                return TRUE;
            }
            InitOnce->Ptr = 0;
            return FALSE;
        case 1: // in progress
            Sleep(1);
            break;
        case 2: // completed
            return TRUE;
        default: // unexpecterd value
            return FALSE;
        }
    }
}

inline void
InitializeConditionVariable_PREVISTA(
    PCONDITION_VARIABLE_PREVISTA ConditionVariable)
{
    ConditionVariable->Target = 0;
    ConditionVariable->Waiters = 0;
    ConditionVariable->Semaphore = CreateSemaphoreW(NULL, 0, MAXLONG, NULL);
    ConditionVariable->ContinueEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
}

inline BOOL
SleepConditionVariableCS_PREVISTA(
    PCONDITION_VARIABLE_PREVISTA cv, PCRITICAL_SECTION cs, DWORD timeout)
{
    DWORD result = 0;

    LeaveCriticalSection(cs);

    InterlockedIncrement((LONG *)&cv->Waiters);
    result = WaitForSingleObject(cv->Semaphore, timeout);

    // send event only on target
    if (InterlockedDecrement((LONG *)&cv->Waiters) == cv->Target
        && result == WAIT_OBJECT_0) {
        SetEvent(cv->ContinueEvent);
    }

    EnterCriticalSection(cs);

    return result;
}

inline void
WakeConditionVariable_PREVISTA(PCONDITION_VARIABLE_PREVISTA ConditionVariable)
{
    if (!ConditionVariable) {
        return;
    }

    if (ConditionVariable->Waiters == 0) {
        return;
    }

    // set target for continue event, alert it on first occurance
    ConditionVariable->Target = ConditionVariable->Waiters - 1;

    SignalObjectAndWait(ConditionVariable->Semaphore,
        ConditionVariable->ContinueEvent, INFINITE, FALSE);
}

#    endif /* _WIN32_WINNT < 0x0600 */

struct sentry__winmutex_s {
    INIT_ONCE init_once;
    CRITICAL_SECTION critical_section;
};

static inline BOOL CALLBACK
sentry__winmutex_initonce(
    PINIT_ONCE UNUSED(InitOnce), PVOID cs, PVOID *UNUSED(lpContext))
{
    InitializeCriticalSection((LPCRITICAL_SECTION)cs);
    return TRUE;
}

static inline void
sentry__winmutex_init(struct sentry__winmutex_s *mutex)
{
    InitOnceExecuteOnce(&mutex->init_once, sentry__winmutex_initonce,
        &mutex->critical_section, NULL);
}

static inline void
sentry__winmutex_lock(struct sentry__winmutex_s *mutex)
{
    InitOnceExecuteOnce(&mutex->init_once, sentry__winmutex_initonce,
        &mutex->critical_section, NULL);
    EnterCriticalSection(&mutex->critical_section);
}

typedef HANDLE sentry_threadid_t;
typedef struct sentry__winmutex_s sentry_mutex_t;
#    define SENTRY__MUTEX_INIT                                                 \
        {                                                                      \
            INIT_ONCE_STATIC_INIT, { 0 }                                       \
        }
#    define sentry__mutex_init(Lock) sentry__winmutex_init(Lock)
#    define sentry__mutex_lock(Lock) sentry__winmutex_lock(Lock)
#    define sentry__mutex_unlock(Lock)                                         \
        LeaveCriticalSection(&(Lock)->critical_section)
#    define sentry__mutex_free(Lock)                                           \
        DeleteCriticalSection(&(Lock)->critical_section)

#    define sentry__thread_init(ThreadId) *ThreadId = INVALID_HANDLE_VALUE
#    define sentry__thread_spawn(ThreadId, Func, Data)                         \
        (*ThreadId = CreateThread(NULL, 0, Func, Data, 0, NULL),               \
            *ThreadId == INVALID_HANDLE_VALUE ? 1 : 0)
#    define sentry__thread_join(ThreadId)                                      \
        WaitForSingleObject(ThreadId, INFINITE)
#    define sentry__thread_free(ThreadId)                                      \
        do {                                                                   \
            if (*ThreadId != INVALID_HANDLE_VALUE) {                           \
                CloseHandle(*ThreadId);                                        \
            }                                                                  \
            *ThreadId = INVALID_HANDLE_VALUE;                                  \
        } while (0)

#    if _WIN32_WINNT < 0x0600
typedef CONDITION_VARIABLE_PREVISTA sentry_cond_t;
#        define sentry__cond_init(CondVar)                                     \
            InitializeConditionVariable_PREVISTA(CondVar)
#        define sentry__cond_wake WakeConditionVariable_PREVISTA
#        define sentry__cond_wait_timeout(CondVar, Lock, Timeout)              \
            SleepConditionVariableCS_PREVISTA(                                 \
                CondVar, &(Lock)->critical_section, Timeout)
#    else
typedef CONDITION_VARIABLE sentry_cond_t;
#        define sentry__cond_init(CondVar) InitializeConditionVariable(CondVar)
#        define sentry__cond_wake WakeConditionVariable
#        define sentry__cond_wait_timeout(CondVar, Lock, Timeout)              \
            SleepConditionVariableCS(                                          \
                CondVar, &(Lock)->critical_section, Timeout)
#    endif
#    define sentry__cond_wait(CondVar, Lock)                                   \
        sentry__cond_wait_timeout(CondVar, Lock, INFINITE)

#else
#    include <errno.h>
#    include <pthread.h>
#    include <sys/time.h>

/* on unix systems signal handlers can interrupt anything which means that
   we're restricted in what we can do.  In particular it's possible that
   we would end up dead locking ourselves.  While we cannot fully prevent
   races we have a logic here that while the signal handler is active we're
   disabling our mutexes so that our signal handler can access what otherwise
   would be protected by the mutex but everyone else needs to wait for the
   signal handler to finish.  This is not without risk because another thread
   might still access what the mutex protects.

   We are thus taking care that whatever such mutexes protect will not make
   us crash under concurrent modifications.  The mutexes we're likely going
   to hit are the options and scope lock. */
bool sentry__block_for_signal_handler(void);
void sentry__enter_signal_handler(void);
void sentry__leave_signal_handler(void);

typedef pthread_t sentry_threadid_t;
typedef pthread_mutex_t sentry_mutex_t;
typedef pthread_cond_t sentry_cond_t;
#    ifdef SENTRY_PLATFORM_LINUX
#        define SENTRY__MUTEX_INIT PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#    else
#        define SENTRY__MUTEX_INIT PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#    endif
#    define sentry__mutex_init(Mutex)                                          \
        do {                                                                   \
            sentry_mutex_t tmp = SENTRY__MUTEX_INIT;                           \
            *(Mutex) = tmp;                                                    \
        } while (0)
#    define sentry__mutex_lock(Mutex)                                          \
        do {                                                                   \
            if (sentry__block_for_signal_handler()) {                          \
                int rv = pthread_mutex_lock(Mutex);                            \
                (void)rv;                                                      \
                assert(rv == 0);                                               \
            }                                                                  \
        } while (0)
#    define sentry__mutex_unlock(Mutex)                                        \
        do {                                                                   \
            if (sentry__block_for_signal_handler()) {                          \
                pthread_mutex_unlock(Mutex);                                   \
            }                                                                  \
        } while (0)
#    define sentry__mutex_free(Lock) pthread_mutex_destroy(Lock)

#    define sentry__cond_init(CondVar)                                         \
        do {                                                                   \
            sentry_cond_t tmp = PTHREAD_COND_INITIALIZER;                      \
            *(CondVar) = tmp;                                                  \
        } while (0)
#    define sentry__cond_wait(Cond, Mutex)                                     \
        do {                                                                   \
            if (sentry__block_for_signal_handler()) {                          \
                pthread_cond_wait(Cond, Mutex);                                \
            }                                                                  \
        } while (0)
#    define sentry__cond_wake pthread_cond_signal
#    define sentry__thread_init(ThreadId)                                      \
        memset(ThreadId, 0, sizeof(sentry_threadid_t))
#    define sentry__thread_spawn(ThreadId, Func, Data)                         \
        (pthread_create(ThreadId, NULL, Func, Data) == 0 ? 0 : 1)
#    define sentry__thread_join(ThreadId) pthread_join(ThreadId, NULL)
#    define sentry__thread_free sentry__thread_init
#    define sentry__threadid_equal pthread_equal
#    define sentry__current_thread pthread_self

static inline int
sentry__cond_wait_timeout(
    sentry_cond_t *cv, sentry_mutex_t *mutex, uint64_t msecs)
{
    if (!sentry__block_for_signal_handler()) {
        return 0;
    }
    struct timeval now;
    struct timespec lock_time;
    gettimeofday(&now, NULL);
    lock_time.tv_sec = now.tv_sec + msecs / 1000ULL;
    lock_time.tv_nsec = (now.tv_usec + 1000ULL * (msecs % 1000)) * 1000ULL;
    return pthread_cond_timedwait(cv, mutex, &lock_time);
}
#endif

static inline long
sentry__atomic_fetch_and_add(volatile long *val, long diff)
{
#ifdef SENTRY_PLATFORM_WINDOWS
#    if SIZEOF_LONG == 8
    return InterlockedExchangeAdd64((LONG64 *)val, diff);
#    else
    return InterlockedExchangeAdd((LONG *)val, diff);
#    endif
#else
    return __atomic_fetch_add(val, diff, __ATOMIC_SEQ_CST);
#endif
}

static inline long
sentry__atomic_store(volatile long *val, long value)
{
#ifdef SENTRY_PLATFORM_WINDOWS
#    if SIZEOF_LONG == 8
    return InterlockedExchange64((LONG64 *)val, value);
#    else
    return InterlockedExchange((LONG *)val, value);
#    endif
#else
    return __atomic_exchange_n(val, value, __ATOMIC_SEQ_CST);
#endif
}

static inline long
sentry__atomic_fetch(volatile long *val)
{
    return sentry__atomic_fetch_and_add(val, 0);
}

struct sentry_bgworker_s;
typedef struct sentry_bgworker_s sentry_bgworker_t;

typedef void (*sentry_task_exec_func_t)(void *task_data, void *state);

/**
 * Creates a new background worker thread.
 *
 * This moves ownership of `state` into the background worker, which uses the
 * given `free_state` function to free that state.
 */
sentry_bgworker_t *sentry__bgworker_new(
    void *state, void (*free_state)(void *state));

/**
 * Returns a reference to the state of the worker.
 */
void *sentry__bgworker_get_state(sentry_bgworker_t *bgw);

/**
 * Drops the reference to the background worker.
 */
void sentry__bgworker_decref(sentry_bgworker_t *bgw);

/**
 * Start a new background worker thread associated with `bgw`.
 * Returns 0 on success.
 */
int sentry__bgworker_start(sentry_bgworker_t *bgw);

/**
 * This will try to shut down the background worker thread, with a `timeout`.
 * Returns 0 on success.
 */
int sentry__bgworker_shutdown(sentry_bgworker_t *bgw, uint64_t timeout);

/**
 * This will set a preferable thread name for background worker.
 * Should be executed before worker start
 */
void sentry__bgworker_setname(sentry_bgworker_t *bgw, const char *thread_name);

/**
 * This will submit a new task to the background thread.
 *
 * Takes ownership of `data`, freeing it using the provided `cleanup_func`.
 * Returns 0 on success.
 */
int sentry__bgworker_submit(sentry_bgworker_t *bgw,
    sentry_task_exec_func_t exec_func, void (*cleanup_func)(void *task_data),
    void *task_data);

/**
 * This function will iterate through all the current tasks of the worker
 * thread, and will call the `callback` function for each task with a matching
 * `exec_func`. The callback can return `true` to indicate if the current task
 * should be dropped from the queue.
 * The function will return the number of dropped tasks.
 */
size_t sentry__bgworker_foreach_matching(sentry_bgworker_t *bgw,
    sentry_task_exec_func_t exec_func,
    bool (*callback)(void *task_data, void *data), void *data);

#endif
