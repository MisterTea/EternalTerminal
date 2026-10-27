#include "sentry_app_hang_latch.h"
#include "sentry_app_hang_monitor.h"
#include "sentry_sync.h"
#include "sentry_testsupport.h"
#include "sentry_thread_stackwalk.h"

SENTRY_TEST(app_hang_should_capture)
{
    // disabled (timeout == 0) -> never captures
    TEST_CHECK(!sentry__app_hang_should_capture(100, 100000, 0, 0));
    // never heartbeated (hb == 0) -> no capture
    TEST_CHECK(!sentry__app_hang_should_capture(0, 100000, 2000, 0));
    // fresh heartbeat (within timeout) -> no capture
    TEST_CHECK(!sentry__app_hang_should_capture(99000, 100000, 2000, 0));
    // stale (now - hb >= timeout) -> capture
    TEST_CHECK(sentry__app_hang_should_capture(98000, 100000, 2000, 0));
    // already fired for this hb -> cooldown, no capture
    TEST_CHECK(!sentry__app_hang_should_capture(98000, 100000, 2000, 98000));
}

SENTRY_TEST(app_hang_latch)
{
#if !SENTRY_HAS_THREAD_STACKWALK
    SKIP_TEST();
#endif
    sentry__app_hang_latch_reset();
    sentry__app_hang_set_active(true);
    sentry_app_hang_latch_t l = sentry__app_hang_current_latch();
    TEST_CHECK(l.target_tid == 0);
    TEST_CHECK(l.last_heartbeat_ms == 0);

    // first heartbeat latches the calling thread + records a timestamp
    sentry_app_hang_heartbeat();
    l = sentry__app_hang_current_latch();
    TEST_CHECK(l.target_tid == sentry__app_hang_current_tid());
    TEST_CHECK(l.target_tid != 0);
    uint64_t first = l.last_heartbeat_ms;
    TEST_CHECK(first != 0);

    sentry__app_hang_latch_reset();
    l = sentry__app_hang_current_latch();
    TEST_CHECK(l.target_tid == 0);
    sentry__app_hang_set_active(false);
}

SENTRY_TEST(app_hang_make_event)
{
    void *ips[2] = { (void *)0x1000, (void *)0x2000 };
    sentry_value_t ev = sentry__app_hang_make_event(ips, 2, 5000);

    sentry_value_t message = sentry_value_get_by_key(ev, "message");
    TEST_CHECK(sentry_value_get_type(message) == SENTRY_VALUE_TYPE_OBJECT);
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(message, "formatted")),
        "App hung for at least 5000 ms.");

    sentry_value_t exc = sentry_value_get_by_index(
        sentry_value_get_by_key(
            sentry_value_get_by_key(ev, "exception"), "values"),
        0);
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(exc, "type")),
        "AppHang");
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(
            sentry_value_get_by_key(exc, "mechanism"), "type")),
        "AppHang");
    TEST_CHECK(sentry_value_is_true(sentry_value_get_by_key(
        sentry_value_get_by_key(exc, "mechanism"), "handled")));
    sentry_value_t frames = sentry_value_get_by_key(
        sentry_value_get_by_key(exc, "stacktrace"), "frames");
    TEST_CHECK(sentry_value_get_length(frames) == 2);

    sentry_value_decref(ev);
}

static long g_app_hang_seen;
static char g_app_hang_type[32];

static size_t
fake_stackwalk(uint64_t tid, void **ips, size_t max)
{
    (void)tid;
    if (max < 2) {
        return 0;
    }
    ips[0] = (void *)0x4000;
    ips[1] = (void *)0x5000;
    return 2;
}

static sentry_value_t
capture_before_send(sentry_value_t event, void *hint, void *data)
{
    (void)hint;
    (void)data;
    sentry_value_t exc = sentry_value_get_by_index(
        sentry_value_get_by_key(
            sentry_value_get_by_key(event, "exception"), "values"),
        0);
    const char *type
        = sentry_value_as_string(sentry_value_get_by_key(exc, "type"));
    if (type) {
        strncpy(g_app_hang_type, type, sizeof(g_app_hang_type) - 1);
    }
    sentry__atomic_store(&g_app_hang_seen, 1);
    sentry_value_decref(event);
    return sentry_value_new_null();
}

SENTRY_TEST(app_hang_monitor_fires)
{
#if !SENTRY_HAS_THREAD_STACKWALK
    SKIP_TEST();
#endif
    g_app_hang_seen = 0;
    g_app_hang_type[0] = '\0';
    sentry__app_hang_latch_reset();
    sentry__app_hang_monitor_set_stackwalk_fn(fake_stackwalk);

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_before_send(options, capture_before_send, NULL);
    sentry_options_set_enable_app_hang_tracking(options, 1);
    sentry_options_set_app_hang_timeout(options, 50);
    sentry_init(options);

    sentry_app_hang_heartbeat();

    for (int i = 0; i < 300 && !sentry__atomic_fetch(&g_app_hang_seen); i++) {
        sleep_ms(10);
    }

    TEST_CHECK(sentry__atomic_fetch(&g_app_hang_seen) == 1);
    TEST_CHECK_STRING_EQUAL(g_app_hang_type, "AppHang");

    sentry_close();
    sentry__app_hang_monitor_set_stackwalk_fn(NULL);
}

SENTRY_TEST(app_hang_disarm_prevents_capture)
{
    // Mirrors app_hang_monitor_fires, but disarms after latching. This is the
    // crash-handler path: once disarmed, the watchdog must not capture an
    // app-hang even though the latched thread stops heart-beating (so a crash
    // is never also reported as an app-hang).
    g_app_hang_seen = 0;
    g_app_hang_type[0] = '\0';
    sentry__app_hang_latch_reset();
    sentry__app_hang_monitor_set_stackwalk_fn(fake_stackwalk);

    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_before_send(options, capture_before_send, NULL);
    sentry_options_set_enable_app_hang_tracking(options, 1);
    sentry_options_set_app_hang_timeout(options, 50);
    sentry_init(options);

    sentry_app_hang_heartbeat();
    // Simulate entering the crash handler: disable -> never heartbeat again.
    sentry__app_hang_set_active(false);

    // Wait well past several timeout/poll cycles to be sure nothing fires.
    for (int i = 0; i < 50; i++) {
        sleep_ms(10);
    }

    TEST_CHECK(sentry__atomic_fetch(&g_app_hang_seen) == 0);

    sentry_close();
    sentry__app_hang_monitor_set_stackwalk_fn(NULL);
}

static long g_real_seen;
static long g_real_frames;
static volatile long g_keep_spinning;

static sentry_value_t
real_before_send(sentry_value_t event, void *hint, void *data)
{
    (void)hint;
    (void)data;
    sentry_value_t exc = sentry_value_get_by_index(
        sentry_value_get_by_key(
            sentry_value_get_by_key(event, "exception"), "values"),
        0);
    sentry_value_t frames = sentry_value_get_by_key(
        sentry_value_get_by_key(exc, "stacktrace"), "frames");
    sentry__atomic_store(&g_real_frames, (long)sentry_value_get_length(frames));
    sentry__atomic_store(&g_real_seen, 1);
    sentry_value_decref(event);
    return sentry_value_new_null();
}

SENTRY_THREAD_FN
spinner(void *arg)
{
    (void)arg;
    sentry_app_hang_heartbeat(); // latch this thread
    while (sentry__atomic_fetch(&g_keep_spinning)) {
        // busy-wait: alive & sampleable but never heartbeats again -> hung
        volatile int x = 0;
        for (int i = 0; i < 100000; i++) {
            x += i;
        }
    }
    return 0;
}

SENTRY_TEST(app_hang_end_to_end)
{
#if !SENTRY_HAS_THREAD_STACKWALK
    SKIP_TEST();
#endif
    g_real_seen = 0;
    g_real_frames = 0;
    sentry__atomic_store(&g_keep_spinning, 1);
    sentry__app_hang_latch_reset();
    sentry__app_hang_monitor_set_stackwalk_fn(NULL); // use the REAL stackwalker

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_before_send(options, real_before_send, NULL);
    sentry_options_set_enable_app_hang_tracking(options, 1);
    sentry_options_set_app_hang_timeout(options, 50);
    sentry_init(options);

    sentry_threadid_t t;
    sentry__thread_spawn(&t, spinner, NULL);

    for (int i = 0; i < 500 && !sentry__atomic_fetch(&g_real_seen); i++) {
        sleep_ms(10);
    }

    sentry__atomic_store(&g_keep_spinning, 0);
    sentry__thread_join(t);

    TEST_CHECK(sentry__atomic_fetch(&g_real_seen) == 1);
    TEST_CHECK(sentry__atomic_fetch(&g_real_frames) > 0);

    sentry_close();
    sentry__app_hang_monitor_set_stackwalk_fn(NULL);
}
