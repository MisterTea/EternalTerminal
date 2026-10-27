#ifndef SENTRY_SYNC_H_INCLUDED
#define SENTRY_SYNC_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_core.h"

#include <assert.h>
#include <stdio.h>

// This is a NOP for platforms that support static mutex initialization.
#define SENTRY__MUTEX_INIT_DYN_ONCE(Mutex) ((void)0)

#ifdef _MSC_VER
#    define THREAD_FUNCTION_API __stdcall
#else
#    define THREAD_FUNCTION_API
#endif

#if defined(__MINGW32__) && !defined(__MINGW64__) && !defined(__clang__)
#    define UNSIGNED_MINGW unsigned
#else
#    define UNSIGNED_MINGW
#endif

// pthreads use `void *` return types, whereas windows uses `DWORD`
#ifdef SENTRY_PLATFORM_WINDOWS
#    define SENTRY_THREAD_FN static UNSIGNED_MINGW DWORD THREAD_FUNCTION_API
#else
#    define SENTRY_THREAD_FN static void *
#endif

// define a recursive mutex for all platforms
#ifdef SENTRY_PLATFORM_WINDOWS
#    if _WIN32_WINNT >= 0x0600
#        include <synchapi.h>
#    endif
#    include <winnt.h>

#    if _WIN32_WINNT < 0x0600

#        define INIT_ONCE_STATIC_INIT { 0 }

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
#    define SENTRY__MUTEX_INIT { INIT_ONCE_STATIC_INIT, { 0 } }
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
#    define sentry__thread_detach(ThreadId) (void)ThreadId
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

static inline sentry_threadid_t
sentry__current_thread(void)
{
    return GetCurrentThread();
}

static inline int
sentry__threadid_equal(sentry_threadid_t a, sentry_threadid_t b)
{
    return GetThreadId(a) == GetThreadId(b);
}

#else
#    include <errno.h>
#    include <pthread.h>
#    include <sys/time.h>

/* on unix systems signal handlers can interrupt anything which means that
   we're restricted in what we can do.  In particular it's possible that
   we would end up dead locking ourselves.  While we cannot fully prevent
   races we have a logic here that while the signal handler is active we're
   disabling our mutexes so that our signal handler can access what
   otherwise would be protected by the mutex but everyone else needs to wait
   for the signal handler to finish.  This is not without risk because
   another thread might still access what the mutex protects.

   We are thus taking care that whatever such mutexes protect will not make
   us crash under concurrent modifications.  The mutexes we're likely going
   to hit are the options and scope lock. */
bool sentry__block_for_signal_handler(void);
/**
 * Enter signal handler context. Returns the recursion depth:
 *   1 = first entry, normal processing
 *   2 = re-entry (crash during handling), skip hooks but try to capture
 *   3+ = multiple re-entries, bail out to previous handler
 */
int sentry__enter_signal_handler(void);
void sentry__leave_signal_handler(void);

typedef pthread_t sentry_threadid_t;
typedef pthread_mutex_t sentry_mutex_t;
typedef pthread_cond_t sentry_cond_t;

#    ifdef SENTRY_PLATFORM_LINUX
#        ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
// In particular musl libc does not define a recursive initializer itself.
// However, we can just define our own. Following the chain of how musl
// initializes its mutex, the attributes are the first member:
// https://git.musl-libc.org/cgit/musl/tree/src/thread/pthread_mutex_init.c#n6
// https://git.musl-libc.org/cgit/musl/tree/src/internal/pthread_impl.h#n86
// https://git.musl-libc.org/cgit/musl/tree/include/alltypes.h.in#n86
#            define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP                     \
                {                                                              \
                    {                                                          \
                        {                                                      \
                            PTHREAD_MUTEX_RECURSIVE                            \
                        }                                                      \
                    }                                                          \
                }
#        endif
#        define SENTRY__MUTEX_INIT PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#    elif defined(SENTRY_PLATFORM_AIX)
// AIX lacks PTHREAD_RECURSIVE_MUTEX_INITIALIZER, though it does have at least
// PTHREAD_MUTEX_INITIALIZER. Unfortunately, it means you must call the mutex
// init function with an initialized mutexattrs set to recursive. This isn't
// workable due to all the mutexes just sitting around not initialized but
// immediately used. The fields are basically guesswork from what happens when
// you initialize the mutex "properly" but changing the bare minimum from a
// static initialization. (Don't ask me what these fields mean, the struct is
// opaquely defined as long[n] fields.)
#        define SENTRY__MUTEX_INIT                                             \
            {                                                                  \
                {                                                              \
                    0, 0, 0, 0, /*_PTH_FLAGS_INIT64*/ 1,                       \
                        PTHREAD_MUTEX_RECURSIVE                                \
                }                                                              \
            }
#    elif defined(__FreeBSD__) || defined(SENTRY_PLATFORM_NX)
// Don't define `SENTRY__MUTEX_INIT` but instead provide a new definition that
// can be used by platforms requiring dynamic recursive mutex initialization.
#        define SENTRY__MUTEX_INIT_DYN(Mutex)                                  \
            static sentry_mutex_t Mutex;                                       \
            static pthread_once_t Mutex##_init_once = PTHREAD_ONCE_INIT;       \
            static void init_##Mutex(void) { sentry__mutex_init(&Mutex); }
#        undef SENTRY__MUTEX_INIT_DYN_ONCE
// Ensures that our dynamically configured mutex is safely initialized once.
#        define SENTRY__MUTEX_INIT_DYN_ONCE(Mutex)                             \
            pthread_once(&Mutex##_init_once, init_##Mutex)
#    else
#        define SENTRY__MUTEX_INIT PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#    endif
#    ifdef SENTRY__MUTEX_INIT_DYN
#        define sentry__mutex_init(Mutex)                                      \
            do {                                                               \
                pthread_mutexattr_t attr;                                      \
                pthread_mutexattr_init(&attr);                                 \
                pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);     \
                pthread_mutex_init(Mutex, &attr);                              \
                pthread_mutexattr_destroy(&attr);                              \
            } while (0)
#    else
#        define sentry__mutex_init(Mutex)                                      \
            do {                                                               \
                sentry_mutex_t tmp = SENTRY__MUTEX_INIT;                       \
                *(Mutex) = tmp;                                                \
            } while (0)
#    endif
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
#    define sentry__thread_detach(ThreadId) pthread_detach(ThreadId)
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
    uint64_t usecs = now.tv_usec + 1000ULL * (msecs % 1000);
    lock_time.tv_sec = now.tv_sec + msecs / 1000ULL + usecs / 1000000ULL;
    lock_time.tv_nsec = (usecs % 1000000ULL) * 1000ULL;
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

/**
 * Compare and swap: atomically compare *val with expected, and if equal,
 * set *val to desired. Returns true if the swap occurred.
 *
 * Uses sequential consistency (ATOMIC_SEQ_CST / InterlockedCompareExchange)
 * to ensure all memory operations are visible across threads. This is
 * appropriate for thread synchronization and state machine transitions.
 *
 * Note: The ABA problem (where a value changes A->B->A between reads) is not
 * a concern for simple integer-based state machines with monotonic transitions.
 */
static inline bool
sentry__atomic_compare_swap(volatile long *val, long expected, long desired)
{
#ifdef SENTRY_PLATFORM_WINDOWS
#    if SIZEOF_LONG == 8
    return InterlockedCompareExchange64((LONG64 *)val, desired, expected)
        == expected;
#    else
    return InterlockedCompareExchange((LONG *)val, desired, expected)
        == expected;
#    endif
#else
    return __atomic_compare_exchange_n(
        val, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#endif
}

/**
 * 64-bit variants of the atomic helpers above. The `long`-based helpers are
 * only 32 bits wide on Windows and 32-bit POSIX targets, so callers that need
 * a full 64-bit atomic (e.g. a monotonic timestamp that must not tear on a
 * 32-bit platform) use these instead.
 */
static inline void
sentry__atomic_store_u64(uint64_t *val, uint64_t value)
{
#ifdef SENTRY_PLATFORM_WINDOWS
    InterlockedExchange64((volatile LONG64 *)val, (LONG64)value);
#else
    __atomic_store_n(val, value, __ATOMIC_SEQ_CST);
#endif
}

static inline uint64_t
sentry__atomic_fetch_u64(uint64_t *val)
{
#ifdef SENTRY_PLATFORM_WINDOWS
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)val, 0, 0);
#else
    return __atomic_load_n(val, __ATOMIC_SEQ_CST);
#endif
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
 * This will try to flush the background worker thread queue, with a `timeout`.
 * Returns 0 on success.
 */
int sentry__bgworker_flush(sentry_bgworker_t *bgw, uint64_t timeout);

/**
 * This will try to shut down the background worker thread, with a `timeout`.
 * Returns 0 on success.
 *
 * The `_cb` variant accepts an `on_timeout` callback that is invoked when
 * the timeout expires, just before detaching the thread. This gives the
 * caller a chance to unblock the worker (e.g. by closing transport handles)
 * so it can finish in-flight work.
 */
int sentry__bgworker_shutdown_cb(sentry_bgworker_t *bgw, uint64_t timeout,
    void (*on_timeout)(void *), void *on_timeout_data);

static inline int
sentry__bgworker_shutdown(sentry_bgworker_t *bgw, uint64_t timeout)
{
    return sentry__bgworker_shutdown_cb(bgw, timeout, NULL, NULL);
}

/**
 * This will set a preferable thread name for background worker.
 * Should be executed before worker start
 */
void sentry__bgworker_setname(sentry_bgworker_t *bgw, const char *thread_name);

#ifdef SENTRY_UNITTEST
/**
 * Test helper function to get the thread name from a bgworker.
 * Only available in unit tests.
 */
const char *sentry__bgworker_get_thread_name(sentry_bgworker_t *bgw);
#endif

/**
 * This will submit a new task to the background thread.
 *
 * The `_delayed` variant delays execution by the specified delay in
 * milliseconds, and the `_at` variant executes after the specified monotonic
 * timestamp. The latter is mostly useful for testing to ensure deterministic
 * ordering of tasks regardless of OS preemption between submissions.
 *
 * Takes ownership of `data`, freeing it using the provided `cleanup_func`.
 * Returns 0 on success.
 */
int sentry__bgworker_submit(sentry_bgworker_t *bgw,
    sentry_task_exec_func_t exec_func, void (*cleanup_func)(void *task_data),
    void *task_data);
int sentry__bgworker_submit_delayed(sentry_bgworker_t *bgw,
    sentry_task_exec_func_t exec_func, void (*cleanup_func)(void *task_data),
    void *task_data, uint64_t delay_ms);
int sentry__bgworker_submit_at(sentry_bgworker_t *bgw,
    sentry_task_exec_func_t exec_func, void (*cleanup_func)(void *task_data),
    void *task_data, uint64_t execute_after);

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

/**
 * A level-triggered waitable atomic flag.
 *
 * The flag has two states: 0 (unset) and 1 (set).
 * - `sentry__waitable_flag_set`: atomically sets the flag to 1 and wakes
 *   a waiting thread.
 * - `sentry__waitable_flag_wait`: waits until the flag is set or the timeout
 *   expires, then atomically clears the flag. Returns true if the flag was
 *   set, false on timeout.
 * - On Windows 8+/Xbox uses WaitOnAddress, on Linux and Android a futex to keep
 *   trigger latency low and pass on wait to the OS. On Darwin it uses
 *   os_sync_wait_on_address when available at runtime. Other POSIX, older
 *   Windows fall back on a sleepy atomic poll loop.
 */

#ifndef __has_include
#    define __has_include(x) 0
#endif

// Compile the Darwin os_sync path only when the SDK has the declarations.
// Call sites below still use __builtin_available so lower deployment targets
// weak-link the symbols and keep using the poll fallback on older OS releases.
#if defined(SENTRY_PLATFORM_DARWIN) && __has_builtin(__builtin_available)      \
    && __has_include(<os/os_sync_wait_on_address.h>)
#    define SENTRY_HAS_DARWIN_OS_SYNC_WAIT_ON_ADDRESS 1
#else
#    define SENTRY_HAS_DARWIN_OS_SYNC_WAIT_ON_ADDRESS 0
#endif

#if defined(SENTRY_PLATFORM_WINDOWS) && _WIN32_WINNT >= 0x0602

typedef struct {
    volatile LONG value;
} sentry_waitable_flag_t;

static inline void
sentry__waitable_flag_init(sentry_waitable_flag_t *flag)
{
    flag->value = 0;
}

static inline void
sentry__waitable_flag_set(sentry_waitable_flag_t *flag)
{
    InterlockedExchange(&flag->value, 1);
    WakeByAddressSingle((PVOID)&flag->value);
}

static inline bool
sentry__waitable_flag_wait(sentry_waitable_flag_t *flag, uint64_t timeout_ms)
{
    LONG undesired = 0;
    // Fast path: flag already set
    if (InterlockedCompareExchange(&flag->value, 0, 1) == 1) {
        return true;
    }
    // WaitOnAddress blocks while *address == undesired
    WaitOnAddress((volatile VOID *)&flag->value, &undesired, sizeof(LONG),
        (DWORD)timeout_ms);
    // Try to consume the flag regardless of wait result
    return InterlockedCompareExchange(&flag->value, 0, 1) == 1;
}

#elif defined(SENTRY_PLATFORM_LINUX)

#    include <linux/futex.h>
#    include <sys/syscall.h>
#    include <unistd.h>

typedef struct {
    volatile int value;
} sentry_waitable_flag_t;

static inline void
sentry__waitable_flag_init(sentry_waitable_flag_t *flag)
{
    flag->value = 0;
}

static inline void
sentry__waitable_flag_set(sentry_waitable_flag_t *flag)
{
    __atomic_store_n(&flag->value, 1, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, &flag->value, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

static inline bool
sentry__waitable_flag_wait(sentry_waitable_flag_t *flag, uint64_t timeout_ms)
{
    // Fast path: flag already set, atomically consume it
    int expected = 1;
    if (__atomic_compare_exchange_n(&flag->value, &expected, 0, false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        return true;
    }

    struct timespec ts;
    ts.tv_sec = (time_t)(timeout_ms / 1000);
    ts.tv_nsec = (long)((timeout_ms % 1000) * 1000000);
    // Block while value == 0
    syscall(SYS_futex, &flag->value, FUTEX_WAIT_PRIVATE, 0, &ts, NULL, 0);
    // Try to consume the flag
    expected = 1;
    return __atomic_compare_exchange_n(
        &flag->value, &expected, 0, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

#elif defined(SENTRY_PLATFORM_DARWIN)                                          \
    && SENTRY_HAS_DARWIN_OS_SYNC_WAIT_ON_ADDRESS

#    include <os/os_sync_wait_on_address.h>
#    include <unistd.h>

#    define SENTRY__DARWIN_OS_SYNC_WAIT_ON_ADDRESS_AVAILABLE()                 \
        __builtin_available(macOS 14.4, iOS 17.4, tvOS 17.4, watchOS 10.4, *)

typedef struct {
    int value;
} sentry_waitable_flag_t;

static inline void
sentry__waitable_flag_init(sentry_waitable_flag_t *flag)
{
    __atomic_store_n(&flag->value, 0, __ATOMIC_SEQ_CST);
}

static inline bool
sentry__waitable_flag_poll_wait(
    sentry_waitable_flag_t *flag, uint64_t timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t deadline_ms = (uint64_t)ts.tv_sec * 1000
        + (uint64_t)ts.tv_nsec / 1000000 + timeout_ms;
    int expected = 1;
    while (!__atomic_compare_exchange_n(&flag->value, &expected, 0, false,
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        expected = 1;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ms
            = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
        if (now_ms >= deadline_ms) {
            return false;
        }
        usleep(10000); // 10ms
    }
    return true;
}

static inline void
sentry__waitable_flag_set(sentry_waitable_flag_t *flag)
{
    __atomic_store_n(&flag->value, 1, __ATOMIC_RELEASE);
    if (SENTRY__DARWIN_OS_SYNC_WAIT_ON_ADDRESS_AVAILABLE()) {
        os_sync_wake_by_address_any(
            &flag->value, sizeof(flag->value), OS_SYNC_WAKE_BY_ADDRESS_NONE);
    }
}

static inline bool
sentry__waitable_flag_wait(sentry_waitable_flag_t *flag, uint64_t timeout_ms)
{
    // Fast path: flag already set, atomically consume it
    int expected = 1;
    if (__atomic_compare_exchange_n(&flag->value, &expected, 0, false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        return true;
    }

    if (SENTRY__DARWIN_OS_SYNC_WAIT_ON_ADDRESS_AVAILABLE()) {
        if (timeout_ms > 0) {
            const uint64_t timeout_ns = timeout_ms > UINT64_MAX / 1000000ULL
                ? UINT64_MAX
                : timeout_ms * 1000000ULL;
            // Block while value == 0
            os_sync_wait_on_address_with_timeout(&flag->value, 0,
                sizeof(flag->value), OS_SYNC_WAIT_ON_ADDRESS_NONE,
                OS_CLOCK_MACH_ABSOLUTE_TIME, timeout_ns);
        }
        // Try to consume the flag
        expected = 1;
        return __atomic_compare_exchange_n(&flag->value, &expected, 0, false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }

    return sentry__waitable_flag_poll_wait(flag, timeout_ms);
}

#elif defined(SENTRY_PLATFORM_WINDOWS)
/* Fallback for older Windows: sleep-poll with Sleep(). */

typedef struct {
    volatile LONG value;
} sentry_waitable_flag_t;

static inline void
sentry__waitable_flag_init(sentry_waitable_flag_t *flag)
{
    InterlockedExchange(&flag->value, 0);
}

static inline void
sentry__waitable_flag_set(sentry_waitable_flag_t *flag)
{
    InterlockedExchange(&flag->value, 1);
}

static inline bool
sentry__waitable_flag_wait(sentry_waitable_flag_t *flag, uint64_t timeout_ms)
{
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout_ms;
    while (!InterlockedCompareExchange(&flag->value, 0, 1)) {
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(10); // 10ms
    }
    return true;
}

#else
/* Fallback for other POSIX platforms (Darwin without os_sync SDK support,
 * FreeBSD, AIX, NX, etc.). Uses a simple sleep-poll loop over an atomic flag.
 * Trades up to 10ms of wake latency for zero platform-specific dependencies.
 */

#    include <unistd.h>

typedef struct {
    volatile int value;
} sentry_waitable_flag_t;

static inline void
sentry__waitable_flag_init(sentry_waitable_flag_t *flag)
{
    __atomic_store_n(&flag->value, 0, __ATOMIC_SEQ_CST);
}

static inline void
sentry__waitable_flag_set(sentry_waitable_flag_t *flag)
{
    __atomic_store_n(&flag->value, 1, __ATOMIC_RELEASE);
}

static inline bool
sentry__waitable_flag_wait(sentry_waitable_flag_t *flag, uint64_t timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t deadline_ms = (uint64_t)ts.tv_sec * 1000
        + (uint64_t)ts.tv_nsec / 1000000 + timeout_ms;
    int expected = 1;
    while (!__atomic_compare_exchange_n(&flag->value, &expected, 0, false,
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        expected = 1;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ms
            = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
        if (now_ms >= deadline_ms) {
            return false;
        }
        usleep(10000); // 10ms
    }
    return true;
}

#endif

#endif
