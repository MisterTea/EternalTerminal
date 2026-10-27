#include "sentry_core.h"
#include "sentry_sync.h"
#include "sentry_testsupport.h"
#include "sentry_utils.h"

struct task_state {
    int executed;
    bool running;
};

static void
task_func(void *data, void *UNUSED(state))
{
    struct task_state *state = data;
    state->executed++;
}

static void
cleanup_func(void *data)
{
    struct task_state *state = data;
    state->running = false;
}

SENTRY_TEST(background_worker)
{
    for (size_t i = 0; i < 100; i++) {
        sentry_bgworker_t *bgw = sentry__bgworker_new(NULL, NULL);
        TEST_ASSERT(!!bgw);

        sentry__bgworker_start(bgw);

        struct task_state ts;
        ts.executed = 0;
        ts.running = true;
        for (size_t j = 0; j < 10; j++) {
            sentry__bgworker_submit(bgw, task_func, cleanup_func, &ts);
        }

        TEST_CHECK_INT_EQUAL(sentry__bgworker_shutdown(bgw, 5000), 0);
        sentry__bgworker_decref(bgw);

        TEST_CHECK_INT_EQUAL(ts.executed, 10);
        TEST_CHECK(!ts.running);
    }
}

static void
sleep_task(void *UNUSED(data), void *UNUSED(state))
{
    sleep_s(1);
}

static sentry_cond_t trailing_task_done;
#ifdef SENTRY__MUTEX_INIT_DYN
SENTRY__MUTEX_INIT_DYN(executed_lock)
#else
static sentry_mutex_t executed_lock = SENTRY__MUTEX_INIT;
#endif

static void
trailing_task(void *data, void *UNUSED(state))
{
    SENTRY__MUTEX_INIT_DYN_ONCE(executed_lock);

    sentry__mutex_lock(&executed_lock);
    bool *executed = (bool *)data;
    *executed = true;
    sentry__mutex_unlock(&executed_lock);
    sentry__cond_wake(&trailing_task_done);
}

static bool
drop_lessthan(void *task, void *data)
{
    return (size_t)task < (size_t)data;
}

static bool
drop_greaterthan(void *task, void *data)
{
    return (size_t)task > (size_t)data;
}

static bool
collect(void *task, void *data)
{
    sentry_value_t *list = (sentry_value_t *)data;
    sentry_value_append(*list, sentry_value_new_int32((int32_t)(size_t)task));
    return true;
}

SENTRY_TEST(task_queue)
{
    SENTRY__MUTEX_INIT_DYN_ONCE(executed_lock);

    sentry__cond_init(&trailing_task_done);
    sentry_bgworker_t *bgw = sentry__bgworker_new(NULL, NULL);
    TEST_ASSERT(!!bgw);
    sentry__bgworker_submit(bgw, sleep_task, NULL, NULL);
    sentry__bgworker_decref(bgw);

    bgw = sentry__bgworker_new(NULL, NULL);
    TEST_ASSERT(!!bgw);

    // submit before starting
    for (size_t i = 0; i < 20; i++) {
        sentry__bgworker_submit(bgw, sleep_task, NULL, (void *)(i % 10));
    }

    sentry__bgworker_start(bgw);

    size_t dropped = 0;
    dropped = sentry__bgworker_foreach_matching(
        bgw, sleep_task, drop_lessthan, (void *)4);
    TEST_CHECK_INT_EQUAL(dropped, 8);
    dropped = sentry__bgworker_foreach_matching(
        bgw, sleep_task, drop_greaterthan, (void *)6);
    TEST_CHECK_INT_EQUAL(dropped, 6);

    int shutdown = sentry__bgworker_shutdown(bgw, 500);
    TEST_CHECK_INT_EQUAL(shutdown, 1);

    // submit another task to the worker which is still in shutdown
    bool executed_after_shutdown = false;
    sentry__bgworker_submit(bgw, trailing_task, NULL, &executed_after_shutdown);

    sentry_value_t list = sentry_value_new_list();
    dropped
        = sentry__bgworker_foreach_matching(bgw, sleep_task, collect, &list);
    TEST_CHECK_INT_EQUAL(dropped, 6);
    TEST_CHECK_JSON_VALUE(list, "[4,5,6,4,5,6]");
    sentry_value_decref(list);

    sentry__bgworker_decref(bgw);

    // wait for the trailing task to be finished
    sentry__mutex_lock(&executed_lock);
    sentry__cond_wait_timeout(&trailing_task_done, &executed_lock, 1000);
    TEST_CHECK(executed_after_shutdown);
}

SENTRY_TEST(bgworker_flush)
{
    sentry_bgworker_t *bgw = sentry__bgworker_new(NULL, NULL);
    TEST_ASSERT(!!bgw);
    sentry__bgworker_submit(bgw, sleep_task, NULL, NULL);

    sentry__bgworker_start(bgw);

    // first flush times out
    int flush = sentry__bgworker_flush(bgw, 500);
    TEST_CHECK_INT_EQUAL(flush, 1);

    // second flush succeeds
    flush = sentry__bgworker_flush(bgw, 1000);
    TEST_CHECK_INT_EQUAL(flush, 0);

    int shutdown = sentry__bgworker_shutdown(bgw, 500);
    TEST_CHECK_INT_EQUAL(shutdown, 0);
    sentry__bgworker_decref(bgw);
}

static void
noop_task(void *UNUSED(data), void *UNUSED(state))
{
}

static void
incr_cleanup(void *data)
{
    (*(int *)data)++;
}

static sentry_cond_t blocker_signal;
#ifdef SENTRY__MUTEX_INIT_DYN
SENTRY__MUTEX_INIT_DYN(blocker_lock)
#else
static sentry_mutex_t blocker_lock = SENTRY__MUTEX_INIT;
#endif
static bool blocker_released;

static void
blocker_task(void *UNUSED(data), void *UNUSED(state))
{
    SENTRY__MUTEX_INIT_DYN_ONCE(blocker_lock);
    sentry__mutex_lock(&blocker_lock);
    while (!blocker_released) {
        sentry__cond_wait_timeout(&blocker_signal, &blocker_lock, 100);
    }
    sentry__mutex_unlock(&blocker_lock);
}

struct order_state {
    int order[10];
    int count;
};

static void
record_order_task(void *data, void *_state)
{
    struct order_state *state = (struct order_state *)_state;
    state->order[state->count++] = (int)(size_t)data;
}

SENTRY_TEST(bgworker_task_delay)
{
    struct order_state os;
    os.count = 0;

    sentry_bgworker_t *bgw = sentry__bgworker_new(&os, NULL);
    TEST_ASSERT(!!bgw);

    uint64_t before = sentry__monotonic_time();
    sentry__bgworker_submit_delayed(
        bgw, record_order_task, NULL, (void *)1, 50);

    sentry__bgworker_start(bgw);
    TEST_CHECK_INT_EQUAL(sentry__bgworker_flush(bgw, 500), 0);
    uint64_t after = sentry__monotonic_time();

    TEST_CHECK_INT_EQUAL(os.count, 1);
    TEST_CHECK_INT_EQUAL(os.order[0], 1);
    TEST_CHECK(after - before >= 50);

    sentry__bgworker_shutdown(bgw, 500);
    sentry__bgworker_decref(bgw);
}

SENTRY_TEST(bgworker_delayed_flush)
{
    struct order_state os;
    os.count = 0;

    sentry_bgworker_t *bgw = sentry__bgworker_new(&os, NULL);
    TEST_ASSERT(!!bgw);

    uint64_t base = sentry__monotonic_time();

    // immediate + eligible delayed + far-future delayed
    sentry__bgworker_submit_at(bgw, record_order_task, NULL, (void *)1, base);
    sentry__bgworker_submit_at(
        bgw, record_order_task, NULL, (void *)2, base + 50);
    sentry__bgworker_submit_delayed(
        bgw, record_order_task, NULL, (void *)3, UINT64_MAX);

    sentry__bgworker_start(bgw);

    // flush covers the immediate and the 50ms task but skips the far-future one
    TEST_CHECK_INT_EQUAL(sentry__bgworker_flush(bgw, 2000), 0);
    TEST_CHECK_INT_EQUAL(os.count, 2);
    TEST_CHECK_INT_EQUAL(os.order[0], 1);
    TEST_CHECK_INT_EQUAL(os.order[1], 2);

    sentry__bgworker_shutdown(bgw, 500);
    sentry__bgworker_decref(bgw);
}

SENTRY_TEST(bgworker_delayed_tasks)
{
    struct order_state os;
    os.count = 0;

    sentry_bgworker_t *bgw = sentry__bgworker_new(&os, NULL);
    TEST_ASSERT(!!bgw);

    // submit_at with a fixed base so ordering is deterministic regardless
    // of OS preemption between submissions (submit_delayed reads the clock
    // per call, so a pause between calls could shift execute_after values
    // and change the expected sort order)
    uint64_t base = sentry__monotonic_time();

    // all tasks sorted by execute_after: immediate (0) first, then delayed
    // by deadline
    //
    // queue after each submit:
    //   i(1)
    //   i(1) d100(3)
    //   i(1) i(6) d100(3)
    //   i(1) i(6) d50(2) d100(3)
    //   i(1) i(6) i(7) d50(2) d100(3)
    //   i(1) i(6) i(7) d50(2) d100(3) d200(5)
    //   i(1) i(6) i(7) d50(2) d100(3) d150(4) d200(5)
    //   i(1) i(6) i(7) i(8) d50(2) d100(3) d150(4) d200(5)
    //   i(1) i(6) i(7) i(8) d50(2) d75(9) d100(3) d150(4) d200(5)
    //   i(1) i(6) i(7) i(8) i(10) d50(2) d75(9) d100(3) d150(4) d200(5)
    sentry__bgworker_submit_at(bgw, record_order_task, NULL, (void *)1, base);
    sentry__bgworker_submit_at(
        bgw, record_order_task, NULL, (void *)3, base + 100);
    sentry__bgworker_submit_at(bgw, record_order_task, NULL, (void *)6, base);
    sentry__bgworker_submit_at(
        bgw, record_order_task, NULL, (void *)2, base + 50);
    sentry__bgworker_submit_at(bgw, record_order_task, NULL, (void *)7, base);
    sentry__bgworker_submit_at(
        bgw, record_order_task, NULL, (void *)5, base + 200);
    sentry__bgworker_submit_at(
        bgw, record_order_task, NULL, (void *)4, base + 150);
    sentry__bgworker_submit_at(bgw, record_order_task, NULL, (void *)8, base);
    sentry__bgworker_submit_at(
        bgw, record_order_task, NULL, (void *)9, base + 75);
    sentry__bgworker_submit_at(bgw, record_order_task, NULL, (void *)10, base);

    sentry__bgworker_start(bgw);
    TEST_CHECK_INT_EQUAL(sentry__bgworker_flush(bgw, 5000), 0);

    // all tasks execute: immediate first, then delayed in deadline order
    TEST_CHECK_INT_EQUAL(os.count, 10);
    TEST_CHECK_INT_EQUAL(os.order[0], 1);
    TEST_CHECK_INT_EQUAL(os.order[1], 6);
    TEST_CHECK_INT_EQUAL(os.order[2], 7);
    TEST_CHECK_INT_EQUAL(os.order[3], 8);
    TEST_CHECK_INT_EQUAL(os.order[4], 10);
    TEST_CHECK_INT_EQUAL(os.order[5], 2);
    TEST_CHECK_INT_EQUAL(os.order[6], 9);
    TEST_CHECK_INT_EQUAL(os.order[7], 3);
    TEST_CHECK_INT_EQUAL(os.order[8], 4);
    TEST_CHECK_INT_EQUAL(os.order[9], 5);

    sentry__bgworker_shutdown(bgw, 500);
    sentry__bgworker_decref(bgw);
}

SENTRY_TEST(bgworker_delayed_priority)
{
    SENTRY__MUTEX_INIT_DYN_ONCE(blocker_lock);
    sentry__cond_init(&blocker_signal);
    blocker_released = false;

    struct order_state os;
    os.count = 0;

    sentry_bgworker_t *bgw = sentry__bgworker_new(&os, NULL);
    TEST_ASSERT(!!bgw);

    // blocker holds the worker busy
    sentry__bgworker_submit(bgw, blocker_task, NULL, NULL);
    // delayed task queued behind the blocker
    sentry__bgworker_submit_delayed(
        bgw, record_order_task, NULL, (void *)1, 50);

    sentry__bgworker_start(bgw);

    // wait for the delayed task to become ready
    sleep_ms(100);

    // submit an immediate task — should NOT bypass the ready delayed task
    sentry__bgworker_submit(bgw, record_order_task, NULL, (void *)2);

    // release the blocker
    sentry__mutex_lock(&blocker_lock);
    blocker_released = true;
    sentry__cond_wake(&blocker_signal);
    sentry__mutex_unlock(&blocker_lock);

    TEST_CHECK_INT_EQUAL(sentry__bgworker_shutdown(bgw, 5000), 0);

    TEST_CHECK_INT_EQUAL(os.count, 2);
    TEST_CHECK_INT_EQUAL(os.order[0], 1); // delayed (was ready first)
    TEST_CHECK_INT_EQUAL(os.order[1], 2); // immediate (submitted later)

    sentry__bgworker_decref(bgw);
}

static void
blocking_record_task(void *data, void *_state)
{
    struct order_state *state = (struct order_state *)_state;
    state->order[state->count++] = (int)(size_t)data;

    SENTRY__MUTEX_INIT_DYN_ONCE(blocker_lock);
    sentry__mutex_lock(&blocker_lock);
    while (!blocker_released) {
        sentry__cond_wait_timeout(&blocker_signal, &blocker_lock, 100);
    }
    sentry__mutex_unlock(&blocker_lock);
}

SENTRY_TEST(bgworker_delayed_current)
{
    SENTRY__MUTEX_INIT_DYN_ONCE(blocker_lock);
    sentry__cond_init(&blocker_signal);
    blocker_released = false;

    struct order_state os;
    os.count = 0;

    sentry_bgworker_t *bgw = sentry__bgworker_new(&os, NULL);
    TEST_ASSERT(!!bgw);

    // head task that blocks and records execution
    sentry__bgworker_submit(bgw, blocking_record_task, NULL, (void *)1);

    sentry__bgworker_start(bgw);
    sleep_ms(100);

    // submit_at(0) would insert before head without the current_task guard
    sentry__bgworker_submit_at(bgw, record_order_task, NULL, (void *)2, 0);

    sentry__mutex_lock(&blocker_lock);
    blocker_released = true;
    sentry__cond_wake(&blocker_signal);
    sentry__mutex_unlock(&blocker_lock);

    TEST_CHECK_INT_EQUAL(sentry__bgworker_shutdown(bgw, 5000), 0);

    // head task must not be re-executed
    TEST_CHECK_INT_EQUAL(os.count, 2);
    TEST_CHECK_INT_EQUAL(os.order[0], 1);
    TEST_CHECK_INT_EQUAL(os.order[1], 2);

    sentry__bgworker_decref(bgw);
}

SENTRY_TEST(bgworker_delayed_head)
{
    struct order_state os;
    os.count = 0;

    sentry_bgworker_t *bgw = sentry__bgworker_new(&os, NULL);
    TEST_ASSERT(!!bgw);

    uint64_t base = sentry__monotonic_time();

    sentry__bgworker_submit_at(bgw, record_order_task, NULL, (void *)1, base);
    sentry__bgworker_submit_at(
        bgw, record_order_task, NULL, (void *)2, base + 1);
    // earlier than first_task -> triggers insert-before-head
    sentry__bgworker_submit_at(
        bgw, record_order_task, NULL, (void *)3, base - 1);

    sentry__bgworker_start(bgw);
    TEST_CHECK_INT_EQUAL(sentry__bgworker_flush(bgw, 5000), 0);

    TEST_CHECK_INT_EQUAL(os.count, 3);
    TEST_CHECK_INT_EQUAL(os.order[0], 3);
    TEST_CHECK_INT_EQUAL(os.order[1], 1);
    TEST_CHECK_INT_EQUAL(os.order[2], 2);

    sentry__bgworker_shutdown(bgw, 500);
    sentry__bgworker_decref(bgw);
}

SENTRY_TEST(bgworker_delayed_drop_current)
{
    SENTRY__MUTEX_INIT_DYN_ONCE(blocker_lock);
    sentry__cond_init(&blocker_signal);
    blocker_released = false;

    struct order_state os;
    os.count = 0;

    sentry_bgworker_t *bgw = sentry__bgworker_new(&os, NULL);
    TEST_ASSERT(!!bgw);

    // A blocks the worker; B is queued behind
    sentry__bgworker_submit(bgw, blocking_record_task, NULL, (void *)1);
    sentry__bgworker_submit(bgw, record_order_task, NULL, (void *)2);

    sentry__bgworker_start(bgw);
    sleep_ms(100);

    // drop the currently executing task A
    sentry__bgworker_foreach_matching(
        bgw, blocking_record_task, drop_lessthan, (void *)2);

    // without the fix, this links to stale current_task
    sentry__bgworker_submit_at(bgw, record_order_task, NULL, (void *)3, 0);

    sentry__mutex_lock(&blocker_lock);
    blocker_released = true;
    sentry__cond_wake(&blocker_signal);
    sentry__mutex_unlock(&blocker_lock);

    TEST_CHECK_INT_EQUAL(sentry__bgworker_shutdown(bgw, 5000), 0);

    TEST_CHECK_INT_EQUAL(os.count, 3);
    TEST_CHECK_INT_EQUAL(os.order[0], 1);
    TEST_CHECK_INT_EQUAL(os.order[1], 3);
    TEST_CHECK_INT_EQUAL(os.order[2], 2);

    sentry__bgworker_decref(bgw);
}

SENTRY_TEST(bgworker_delayed_drop_next)
{
    SENTRY__MUTEX_INIT_DYN_ONCE(blocker_lock);
    sentry__cond_init(&blocker_signal);
    blocker_released = false;

    struct order_state os;
    os.count = 0;

    sentry_bgworker_t *bgw = sentry__bgworker_new(&os, NULL);
    TEST_ASSERT(!!bgw);

    // A blocks the worker; B and C are queued behind
    sentry__bgworker_submit(bgw, blocking_record_task, NULL, (void *)1);
    sentry__bgworker_submit(bgw, record_order_task, NULL, (void *)2);
    sentry__bgworker_submit(bgw, record_order_task, NULL, (void *)3);

    sentry__bgworker_start(bgw);
    sleep_ms(100);

    // drop B (next after current_task A)
    sentry__bgworker_foreach_matching(
        bgw, record_order_task, drop_lessthan, (void *)3);

    // walks from current_task->next_task which must be valid
    sentry__bgworker_submit_at(bgw, record_order_task, NULL, (void *)4, 0);

    sentry__mutex_lock(&blocker_lock);
    blocker_released = true;
    sentry__cond_wake(&blocker_signal);
    sentry__mutex_unlock(&blocker_lock);

    TEST_CHECK_INT_EQUAL(sentry__bgworker_shutdown(bgw, 5000), 0);

    TEST_CHECK_INT_EQUAL(os.count, 3);
    TEST_CHECK_INT_EQUAL(os.order[0], 1);
    TEST_CHECK_INT_EQUAL(os.order[1], 4);
    TEST_CHECK_INT_EQUAL(os.order[2], 3);

    sentry__bgworker_decref(bgw);
}

SENTRY_TEST(bgworker_delayed_cleanup)
{
    int cleaned = 0;

    sentry_bgworker_t *bgw = sentry__bgworker_new(NULL, NULL);
    TEST_ASSERT(!!bgw);

    // immediate tasks (cleanup after execution)
    sentry__bgworker_submit(bgw, noop_task, incr_cleanup, &cleaned);
    sentry__bgworker_submit(bgw, noop_task, incr_cleanup, &cleaned);

    // far-future delayed tasks (discarded on shutdown, cleanup in decref)
    sentry__bgworker_submit_at(
        bgw, noop_task, incr_cleanup, &cleaned, UINT64_MAX);
    sentry__bgworker_submit_at(
        bgw, noop_task, incr_cleanup, &cleaned, UINT64_MAX);
    sentry__bgworker_submit_at(
        bgw, noop_task, incr_cleanup, &cleaned, UINT64_MAX);

    sentry__bgworker_start(bgw);
    TEST_CHECK_INT_EQUAL(sentry__bgworker_shutdown(bgw, 1000), 0);
    sentry__bgworker_decref(bgw);

    TEST_CHECK_INT_EQUAL(cleaned, 5);
}

SENTRY_TEST(bgworker_delayed_shutdown)
{
    struct order_state os;
    os.count = 0;

    sentry_bgworker_t *bgw = sentry__bgworker_new(&os, NULL);
    TEST_ASSERT(!!bgw);

    // immediate tasks
    sentry__bgworker_submit(bgw, record_order_task, NULL, (void *)1);
    sentry__bgworker_submit(bgw, record_order_task, NULL, (void *)2);
    sentry__bgworker_submit(bgw, record_order_task, NULL, (void *)3);

    // pending delayed tasks are discarded on shutdown
    sentry__bgworker_submit_at(
        bgw, record_order_task, NULL, (void *)4, UINT64_MAX);
    sentry__bgworker_submit_at(
        bgw, record_order_task, NULL, (void *)5, UINT64_MAX);
    sentry__bgworker_submit_at(
        bgw, record_order_task, NULL, (void *)6, UINT64_MAX);

    sentry__bgworker_start(bgw);
    TEST_CHECK_INT_EQUAL(sentry__bgworker_shutdown(bgw, 1000), 0);

    TEST_CHECK_INT_EQUAL(os.count, 3);
    TEST_CHECK_INT_EQUAL(os.order[0], 1);
    TEST_CHECK_INT_EQUAL(os.order[1], 2);
    TEST_CHECK_INT_EQUAL(os.order[2], 3);

    sentry__bgworker_decref(bgw);
}

SENTRY_TEST(cond_wait_timeout_overflow)
{
#if !(defined(SENTRY_PLATFORM_MACOS)                                           \
    || (defined(SENTRY_PLATFORM_LINUX) && !defined(SENTRY_PLATFORM_ANDROID)))
    SKIP_TEST();
#else
    sentry_cond_t cond;
    sentry_mutex_t mutex;
    sentry__cond_init(&cond);
    sentry__mutex_init(&mutex);

    // Opportunistic guard: in the vulnerable time window (tv_usec >= 750000),
    // old code returned EINVAL immediately and callers could busy-spin.
    sentry__mutex_lock(&mutex);
    TEST_CHECK_INT_EQUAL(
        sentry__cond_wait_timeout(&cond, &mutex, 250), ETIMEDOUT);
    sentry__mutex_unlock(&mutex);

    sentry__mutex_free(&mutex);
    pthread_cond_destroy(&cond);
#endif
}
