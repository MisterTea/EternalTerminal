#include "sentry_alloc.h"
#include "sentry_backend.h"
#include "sentry_core.h"
#include "sentry_database.h"
#include "sentry_options.h"
#include "sentry_string.h"
#include "sentry_sync.h"
#include "sentry_testsupport.h"
#include "sentry_transport.h"
#include "transports/sentry_http_transport.h"

static void
send_envelope_test_basic(sentry_envelope_t *envelope, void *data)
{
    uint64_t *called = data;
    *called += 1;

    sentry_value_t event = sentry_envelope_get_event(envelope);
    TEST_CHECK(!sentry_value_is_null(event));
    const char *event_id
        = sentry_value_as_string(sentry_value_get_by_key(event, "event_id"));
    TEST_CHECK_STRING_EQUAL(event_id, "4c035723-8638-4c3a-923f-2ab9d08b4018");

    if (*called == 1) {
        const char *msg = sentry_value_as_string(sentry_value_get_by_key(
            sentry_value_get_by_key(event, "message"), "formatted"));
        TEST_CHECK_STRING_EQUAL(msg, "Hello World!");
        const char *release
            = sentry_value_as_string(sentry_value_get_by_key(event, "release"));
        TEST_CHECK_STRING_EQUAL(release, "prod");
        const char *trans = sentry_value_as_string(
            sentry_value_get_by_key(event, "transaction"));
        TEST_CHECK_STRING_EQUAL(trans, "demo-trans");
    }
    sentry_envelope_free(envelope);
}

SENTRY_TEST(basic_function_transport)
{
    uint64_t called = 0;
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_transport_t *transport
        = sentry_transport_new(send_envelope_test_basic);
    sentry_transport_set_state(transport, &called);
    sentry_options_set_transport(options, transport);
    sentry_options_set_release(options, "prod");
    sentry_options_set_require_user_consent(options, true);
    sentry_init(options);

    sentry_set_transaction("demo-trans");

    sentry_capture_event(sentry_value_new_message_event(
        SENTRY_LEVEL_INFO, "root", "not captured due to missing consent"));
    sentry_user_consent_give();

    sentry_capture_event(sentry_value_new_message_event(
        SENTRY_LEVEL_INFO, "root", "Hello World!"));

    sentry_value_t obj = sentry_value_new_object();
    // something that is not a UUID, as this will be forcibly changed
    sentry_value_set_by_key(obj, "event_id", sentry_value_new_int32(1234));
    sentry_capture_event(obj);

    sentry_user_consent_revoke();
    sentry_capture_event(sentry_value_new_message_event(SENTRY_LEVEL_INFO,
        "root", "not captured either due to revoked consent"));

    sentry_close();

    TEST_CHECK_INT_EQUAL(called, 2);
}

static void
counting_transport_func(sentry_envelope_t *envelope, void *data)
{
    uint64_t *called = data;
    *called += 1;
    sentry_envelope_free(envelope);
}

static sentry_value_t
before_send(sentry_value_t event, void *UNUSED(hint), void *data)
{
    uint64_t *called = data;
    *called += 1;

    return event;
}

SENTRY_TEST(sampling_before_send)
{
    uint64_t called_beforesend = 0;
    uint64_t called_transport = 0;

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_transport_t *transport
        = sentry_transport_new(counting_transport_func);
    sentry_transport_set_state(transport, &called_transport);
    sentry_options_set_transport(options, transport);
    sentry_options_set_before_send(options, before_send, &called_beforesend);
    sentry_options_set_sample_rate(options, 0.75);
    sentry_init(options);

    for (int i = 0; i < 100; i++) {
        sentry_capture_event(
            sentry_value_new_message_event(SENTRY_LEVEL_INFO, NULL, "foo"));
    }

    sentry_close();

    // The behavior here has changed with version 0.4.19:
    // the documentation (https://develop.sentry.dev/sdk/sessions/#filter-order)
    // requires that the sampling-rate filter for all SDKs is executed last.
    // This means the `before_send` callback will be invoked every time and only
    // the actual transport will be randomly sampled.
    TEST_CHECK(called_transport > 50 && called_transport < 100);
    TEST_CHECK_INT_EQUAL(called_beforesend, 100);
}

static sentry_value_t
discarding_before_send(sentry_value_t event, void *UNUSED(hint), void *data)
{
    uint64_t *called = data;
    *called += 1;

    sentry_value_decref(event);
    return sentry_value_new_null();
}

SENTRY_TEST(discarding_before_send)
{
    uint64_t called_beforesend = 0;
    uint64_t called_transport = 0;

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    // Disable sessions or this test would fail if env:SENTRY_RELEASE is set.
    sentry_options_set_auto_session_tracking(options, 0);
    sentry_transport_t *transport
        = sentry_transport_new(counting_transport_func);
    sentry_transport_set_state(transport, &called_transport);
    sentry_options_set_transport(options, transport);
    sentry_options_set_before_send(
        options, discarding_before_send, &called_beforesend);
    sentry_init(options);

    sentry_capture_event(
        sentry_value_new_message_event(SENTRY_LEVEL_INFO, NULL, "foo"));

    sentry_close();

    TEST_CHECK_INT_EQUAL(called_transport, 0);
    TEST_CHECK_INT_EQUAL(called_beforesend, 1);
}

SENTRY_TEST(crash_marker)
{
    // We don't use sentry_init() in this test so we must create a database dir
    sentry_path_t *database_path
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".sentry-native");
    TEST_ASSERT(!!database_path);
    TEST_ASSERT(!sentry__path_create_dir_all(database_path));

    SENTRY_TEST_OPTIONS_NEW(options);
#ifdef SENTRY_PLATFORM_WINDOWS
    sentry_options_set_database_pathw(options, database_path->path_w);
#else
    sentry_options_set_database_path(options, database_path->path);
#endif

    // There is no marker in the beginning, but clearing returns true if the
    // marker doesn't exist (i.e., we get an `ENOENT` or `ERROR_FILE_NOT_FOUND`)
    TEST_CHECK(sentry__clear_crash_marker(options));
    // We can also verify this with has_crash_marker
    TEST_CHECK(!sentry__has_crash_marker(options));

    TEST_CHECK(sentry__write_crash_marker(options));
    TEST_CHECK(sentry__has_crash_marker(options));
    TEST_CHECK(sentry__write_crash_marker(options));
    TEST_CHECK(sentry__has_crash_marker(options));

    TEST_CHECK(sentry__clear_crash_marker(options));
    TEST_CHECK(!sentry__has_crash_marker(options));
    TEST_CHECK(sentry__clear_crash_marker(options));

    sentry_options_free(options);

    sentry__path_remove_all(database_path);
    sentry__path_free(database_path);
}

SENTRY_TEST(crashed_last_run)
{
    // fails before init() is called
    TEST_CHECK_INT_EQUAL(sentry_clear_crashed_last_run(), 1);

    // clear any leftover from previous test runs
    {
        SENTRY_TEST_OPTIONS_NEW(options);
        sentry__clear_crash_marker(options);
        sentry_options_free(options);
    }

    const char dsn[] = { 'h', 't', 't', 'p', 's', ':', '/', '/', 'f', 'o', 'o',
        '@', 's', 'e', 'n', 't', 'r', 'y', '.', 'i', 'n', 'v', 'a', 'l', 'i',
        'd', '/', '4', '2' };

    {
        const char *dsn_str = "https://foo@sentry.invalid/42";
        SENTRY_TEST_OPTIONS_NEW(options);
        sentry_options_set_dsn_n(options, dsn, sizeof(dsn));
        TEST_CHECK_STRING_EQUAL(sentry_options_get_dsn(options), dsn_str);
        TEST_CHECK_INT_EQUAL(sentry_init(options), 0);
        sentry_close();

        TEST_CHECK_INT_EQUAL(sentry_get_crashed_last_run(), 0);
    }

    {
        SENTRY_TEST_OPTIONS_NEW(options);
        sentry_options_set_dsn_n(options, dsn, sizeof(dsn));

        // simulate a crash
        TEST_CHECK(sentry__write_crash_marker(options));

        TEST_CHECK_INT_EQUAL(sentry_init(options), 0);

        TEST_CHECK_INT_EQUAL(sentry_get_crashed_last_run(), 1);

        // clear the status and re-init
        TEST_CHECK_INT_EQUAL(sentry_clear_crashed_last_run(), 0);

        sentry_close();

        // no change yet before sentry_init() is called
        TEST_CHECK_INT_EQUAL(sentry_get_crashed_last_run(), 1);
    }

    {
        SENTRY_TEST_OPTIONS_NEW(options);
        sentry_options_set_dsn_n(options, dsn, sizeof(dsn));
        TEST_CHECK_INT_EQUAL(sentry_init(options), 0);
        sentry_close();

        TEST_CHECK_INT_EQUAL(sentry_get_crashed_last_run(), 0);
    }
}

SENTRY_TEST(capture_minidump_basic)
{
    // skipping on platforms that don't have access to fixtures on the local FS
#if defined(SENTRY_PLATFORM_ANDROID) || defined(SENTRY_PLATFORM_NX)            \
    || defined(SENTRY_PLATFORM_PS) || defined(SENTRY_PLATFORM_XBOX)
    SKIP_TEST();
#else
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

    const char *minidump_rel_path = "../fixtures/minidump.dmp";
    sentry_path_t *path = sentry__path_from_str(__FILE__);
    sentry_path_t *dir = sentry__path_dir(path);
    sentry_path_t *minidump_path
        = sentry__path_join_str(dir, minidump_rel_path);

    const sentry_uuid_t event_id = sentry_capture_minidump(minidump_path->path);
    TEST_CHECK(!sentry_uuid_is_nil(&event_id));

    sentry__path_free(minidump_path);
    sentry__path_free(dir);
    sentry__path_free(path);

    sentry_close();
#endif
}

SENTRY_TEST(capture_minidump_wide)
{
#if !defined(SENTRY_PLATFORM_WINDOWS) || defined(SENTRY_PLATFORM_XBOX)
    SKIP_TEST();
#else
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

    const wchar_t *minidump_rel_path = L"../fixtures/minidump.dmp";
    sentry_path_t *path = sentry__path_from_str(__FILE__);
    sentry_path_t *dir = sentry__path_dir(path);
    sentry_path_t *minidump_path
        = sentry__path_join_wstr(dir, minidump_rel_path);

    const sentry_uuid_t event_id
        = sentry_capture_minidumpw(minidump_path->path_w);
    TEST_CHECK(!sentry_uuid_is_nil(&event_id));

    sentry__path_free(minidump_path);
    sentry__path_free(dir);
    sentry__path_free(path);

    sentry_close();
#endif
}

SENTRY_TEST(capture_minidump_null_path)
{
    // a NULL path will activate the path check at the beginning of the function
    const sentry_uuid_t event_id = sentry_capture_minidump(NULL);
    TEST_CHECK(sentry_uuid_is_nil(&event_id));
}

SENTRY_TEST(capture_minidump_without_sentry_init)
{
    // if the path initialization was successful, but the SDK wasn't
    // initialized, capturing will fail at the point of acquiring the active
    // options.
    const sentry_uuid_t event_id
        = sentry_capture_minidump("irrelevant_minidump_path");
    TEST_CHECK(sentry_uuid_is_nil(&event_id));
}

SENTRY_TEST(capture_minidump_invalid_path)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

    // here the initialization is successful, but we provide an invalid minidump
    // path which should prevent capture locally and return a nil UUID since we
    // cannot create an attachment envelope-item for the minidump file.
    const sentry_uuid_t event_id
        = sentry_capture_minidump("some_invalid_minidump_path");
    TEST_CHECK(sentry_uuid_is_nil(&event_id));

    sentry_close();
}

SENTRY_TEST(capture_minidump_discard)
{
    // skipping on platforms that don't have access to fixtures on the local FS
#if defined(SENTRY_PLATFORM_ANDROID) || defined(SENTRY_PLATFORM_NX)            \
    || defined(SENTRY_PLATFORM_PS) || defined(SENTRY_PLATFORM_XBOX)
    SKIP_TEST();
#else
    uint64_t called_beforesend = 0;

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_before_send(
        options, discarding_before_send, &called_beforesend);
    sentry_init(options);

    const char *minidump_rel_path = "../fixtures/minidump.dmp";
    sentry_path_t *path = sentry__path_from_str(__FILE__);
    sentry_path_t *dir = sentry__path_dir(path);
    sentry_path_t *minidump_path
        = sentry__path_join_str(dir, minidump_rel_path);

    const sentry_uuid_t event_id = sentry_capture_minidump(minidump_path->path);
    TEST_CHECK(sentry_uuid_is_nil(&event_id));
    TEST_CHECK_INT_EQUAL(called_beforesend, 1);

    sentry__path_free(minidump_path);
    sentry__path_free(dir);
    sentry__path_free(path);

    sentry_close();
#endif
}

SENTRY_TEST(basic_transport_thread_name)
{
#if defined(SENTRY_PLATFORM_NX)
    // NX transport won't start without custom network_connect_func.
    SKIP_TEST();
#endif

    const char *expected_thread_name = "sentry::worker_thread";

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_transport_thread_name(options, expected_thread_name);

    // Initialize sentry which should start the transport and set the thread
    // name
    TEST_CHECK_INT_EQUAL(sentry_init(options), 0);

    // Access the transport through runtime options to check if thread name was
    // set
    SENTRY_WITH_OPTIONS (runtime_options) {
        TEST_ASSERT(!!runtime_options->transport);

        // Get the bgworker from the transport (for HTTP transports)
        sentry_bgworker_t *bgworker
            = (sentry_bgworker_t *)sentry__http_transport_get_bgworker(
                runtime_options->transport);
        TEST_ASSERT(!!bgworker);

        // Check if the thread name was properly set on the bgworker
        const char *actual_thread_name
            = sentry__bgworker_get_thread_name(bgworker);

        if (actual_thread_name) {
            TEST_CHECK_STRING_EQUAL(actual_thread_name, expected_thread_name);
        } else {
            TEST_CHECK(false); // Fail if thread_name is NULL
            TEST_MSG("Transport thread name was not set ");
        }
    }

    sentry_close();
}

SENTRY_TEST(installation_id)
{
    // no DSN -> installation ID is generated
    SENTRY_TEST_OPTIONS_NEW(opts0);
    sentry_init(opts0);
    SENTRY_WITH_OPTIONS (options) {
        TEST_ASSERT(!!options->run->installation_id);
        TEST_CHECK_INT_EQUAL(strlen(options->run->installation_id), 36);
    }
    sentry_close();

    // DSN A -> installation ID is generated
    SENTRY_TEST_OPTIONS_NEW(opts1);
    sentry_options_set_dsn(opts1, "http://keya@127.0.0.1/42");
    sentry_init(opts1);
    char *id_a = NULL;
    SENTRY_WITH_OPTIONS (options) {
        TEST_ASSERT(!!options->run->installation_id);
        TEST_CHECK_INT_EQUAL(strlen(options->run->installation_id), 36);
        id_a = sentry__string_clone(options->run->installation_id);
    }
    sentry_close();

    // same DSN A -> installation ID persists
    SENTRY_TEST_OPTIONS_NEW(opts2);
    sentry_options_set_dsn(opts2, "http://keya@127.0.0.1/42");
    sentry_init(opts2);
    SENTRY_WITH_OPTIONS (options) {
        TEST_ASSERT(!!options->run->installation_id);
        TEST_CHECK_STRING_EQUAL(options->run->installation_id, id_a);
    }
    sentry_close();

    // different DSN B -> installation ID rotates
    SENTRY_TEST_OPTIONS_NEW(opts3);
    sentry_options_set_dsn(opts3, "http://keyb@127.0.0.1/42");
    sentry_init(opts3);
    SENTRY_WITH_OPTIONS (options) {
        TEST_ASSERT(!!options->run->installation_id);
        TEST_CHECK(strcmp(options->run->installation_id, id_a) != 0);
    }
    sentry_close();

    sentry_free(id_a);
}

static void
backend_free_options_ref(sentry_backend_t *backend)
{
    const sentry_options_t **options_ref = backend->data;
    *options_ref = sentry__options_getref();
}

SENTRY_TEST(clear_options)
{
    const sentry_options_t *options_ref = NULL;

    SENTRY_TEST_OPTIONS_NEW(options);

    sentry_backend_t *backend = SENTRY_MAKE(sentry_backend_t);
    TEST_ASSERT(!!backend);
    backend->free_func = backend_free_options_ref;
    backend->data = &options_ref;
    sentry_options_set_backend(options, backend);

    sentry_init(options);
    sentry_close();

    // The backend free hook runs from sentry_options_free(). At that point,
    // sentry__options_getref() must no longer expose the options.
    TEST_CHECK(options_ref == NULL);
}
