/**
 * Test fixture for stressing the inproc backend with concurrent crashes.
 *
 * This test spawns multiple threads that all crash concurrently to expose
 * race conditions in the signal handler / handler thread synchronization.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#    include <pthread.h>
#    include <unistd.h>
#    define sleep_ms(ms) usleep((ms) * 1000)
#else
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#    define sleep_ms(ms) Sleep(ms)
#endif

#define CRASH_THREADS 20

static void *invalid_mem = (void *)1;

// Barrier for synchronizing threads
#ifndef _WIN32
static volatile int g_barrier = 0;
static volatile int g_ready_count = 0;
#else
static volatile LONG g_barrier = 0;
static volatile LONG g_ready_count = 0;
#endif

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
void
do_crash(void)
{
    memset((char *)invalid_mem, 1, 100);
}

static void
wait_at_barrier(void)
{
#ifndef _WIN32
    __atomic_add_fetch(&g_ready_count, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&g_barrier, __ATOMIC_SEQ_CST) == 0) {
        // spin
    }
#else
    InterlockedIncrement(&g_ready_count);
    while (InterlockedCompareExchange(&g_barrier, 1, 1) == 0) {
        // spin
    }
#endif
}

static void
release_barrier(void)
{
#ifndef _WIN32
    __atomic_store_n(&g_barrier, 1, __ATOMIC_SEQ_CST);
#else
    InterlockedExchange(&g_barrier, 1);
#endif
}

static int
all_threads_ready(void)
{
#ifndef _WIN32
    return __atomic_load_n(&g_ready_count, __ATOMIC_SEQ_CST) == CRASH_THREADS;
#else
    return InterlockedCompareExchange(
               &g_ready_count, CRASH_THREADS, CRASH_THREADS)
        == CRASH_THREADS;
#endif
}

#ifndef _WIN32
static void *
crash_thread(void *param)
{
    (void)param;
    wait_at_barrier();
    do_crash();
    return NULL;
}

void
run_concurrent_crash(void)
{
    pthread_t threads[CRASH_THREADS];

    for (int i = 0; i < CRASH_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, crash_thread, NULL) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            exit(1);
        }
    }

    while (!all_threads_ready()) {
        sleep_ms(1);
    }
    sleep_ms(10);

    release_barrier();

    for (int i = 0; i < CRASH_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
}
#else
static DWORD WINAPI
crash_thread(LPVOID param)
{
    (void)param;
    wait_at_barrier();
    do_crash();
    return 0;
}

void
run_concurrent_crash(void)
{
    HANDLE threads[CRASH_THREADS];

    for (int i = 0; i < CRASH_THREADS; i++) {
        threads[i] = CreateThread(NULL, 0, crash_thread, NULL, 0, NULL);
        if (!threads[i]) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            exit(1);
        }
    }

    while (!all_threads_ready()) {
        sleep_ms(1);
    }
    sleep_ms(10);

    release_barrier();

    WaitForMultipleObjects(CRASH_THREADS, threads, TRUE, 5000);
    for (int i = 0; i < CRASH_THREADS; i++) {
        CloseHandle(threads[i]);
    }
}
#endif
