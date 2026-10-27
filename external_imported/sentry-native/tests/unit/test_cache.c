#include "sentry_core.h"
#include "sentry_database.h"
#include "sentry_envelope.h"
#include "sentry_options.h"
#include "sentry_path.h"
#include "sentry_retry.h"
#include "sentry_string.h"
#include "sentry_testsupport.h"
#include "sentry_uuid.h"
#include "sentry_value.h"

#ifdef SENTRY_PLATFORM_WINDOWS
#    include <windows.h>
#elif !defined(SENTRY_PLATFORM_NX) && !defined(SENTRY_PLATFORM_PS)
#    include <unistd.h>
#    include <utime.h>
#endif

static int
set_file_mtime(const sentry_path_t *path, time_t mtime)
{
#ifdef SENTRY_PLATFORM_WINDOWS
    HANDLE h = CreateFileW(path->path_w, FILE_WRITE_ATTRIBUTES, 0, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    // 100 ns intervals since January 1, 1601 (UTC)
    uint64_t t = ((uint64_t)mtime * 10000000ULL) + 116444736000000000ULL;
    FILETIME ft = { (DWORD)t, (DWORD)(t >> 32) };
    BOOL rv = SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
    return rv ? 0 : -1;
#elif !defined(SENTRY_PLATFORM_NX) && !defined(SENTRY_PLATFORM_PS)
    struct utimbuf times = { .modtime = mtime, .actime = mtime };
    return utime(path->path, &times);
#else
    (void)path;
    (void)mtime;
    return -1;
#endif
}

SENTRY_TEST(cache_keep)
{
#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#endif
    SENTRY_TEST_OPTIONS_NEW(options);
    TEST_ASSERT(!!options->transport);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_cache_keep(options, true);
    sentry_options_set_http_retry(options, false);
    sentry_init(options);

    sentry_path_t *cache_path
        = sentry__path_join_str(options->database_path, "cache");
    TEST_ASSERT(!!cache_path);
    TEST_ASSERT(sentry__path_remove_all(cache_path) == 0);

    sentry_path_t *old_run_path
        = sentry__path_join_str(options->database_path, "old.run");
    TEST_ASSERT(!!old_run_path);
    TEST_ASSERT(sentry__path_create_dir_all(old_run_path) == 0);

    sentry_envelope_t *envelope = sentry__envelope_new();
    TEST_ASSERT(!!envelope);
    sentry_uuid_t event_id = sentry_uuid_new_v4();
    sentry_value_t event = sentry__value_new_event_with_id(&event_id);
    sentry__envelope_add_event(envelope, event);

    char *envelope_filename = sentry__uuid_as_filename(&event_id, ".envelope");
    TEST_ASSERT(!!envelope_filename);
    sentry_path_t *old_envelope_path
        = sentry__path_join_str(old_run_path, envelope_filename);
    TEST_ASSERT(
        sentry_envelope_write_to_path(envelope, old_envelope_path) == 0);
    sentry_envelope_free(envelope);

    sentry_path_t *cached_envelope_path
        = sentry__path_join_str(cache_path, envelope_filename);
    TEST_ASSERT(!!cached_envelope_path);

    TEST_ASSERT(sentry__path_is_file(old_envelope_path));
    TEST_ASSERT(!sentry__path_is_file(cached_envelope_path));

    sentry__process_old_runs(options, 0);
    sentry_flush(5000);

    TEST_ASSERT(!sentry__path_is_file(old_envelope_path));
    TEST_ASSERT(sentry__path_is_file(cached_envelope_path));

    sentry__path_free(old_envelope_path);
    sentry__path_free(cached_envelope_path);
    sentry__path_free(old_run_path);
    sentry__path_free(cache_path);
    sentry_free(envelope_filename);
    sentry_close();
}

SENTRY_TEST(cache_max_size)
{
#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#endif
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_cache_keep(options, true);
    sentry_options_set_cache_max_size(options, 10 * 1024); // 10 kb
    sentry_init(options);

    sentry_path_t *cache_path
        = sentry__path_join_str(options->database_path, "cache");
    TEST_ASSERT(!!cache_path);
    TEST_ASSERT(sentry__path_remove_all(cache_path) == 0);
    TEST_ASSERT(sentry__path_create_dir_all(cache_path) == 0);

    // Oldest entry is a retry-format envelope with a cache-sibling
    // sibling. Pruning must remove the sibling too — otherwise the sibling
    // would survive as an orphan and the cache_count assertion below trips.
    sentry_uuid_t retry_id = sentry_uuid_new_v4();
    char retry_uuid[37];
    sentry_uuid_as_string(&retry_id, retry_uuid);
    char retry_name[128];
    snprintf(retry_name, sizeof(retry_name), "1700000000000-00-%.36s.envelope",
        retry_uuid);
    sentry_path_t *retry_path = sentry__path_join_str(cache_path, retry_name);
    TEST_ASSERT(sentry__path_touch(retry_path) == 0);
    char sib_name[128];
    snprintf(sib_name, sizeof(sib_name), "%.36s-payload.bin", retry_uuid);
    sentry_path_t *sib_path = sentry__path_join_str(cache_path, sib_name);
    TEST_ASSERT(sentry__path_write_buffer(sib_path, "data", 4) == 0);
    TEST_ASSERT(set_file_mtime(retry_path, time(NULL) - 3600) == 0);

    // 10 x 5 kb files
    for (int i = 0; i < 10; i++) {
        sentry_uuid_t event_id = sentry_uuid_new_v4();
        char *filename = sentry__uuid_as_filename(&event_id, ".envelope");
        TEST_ASSERT(!!filename);
        sentry_path_t *filepath = sentry__path_join_str(cache_path, filename);
        sentry_free(filename);

        sentry_filewriter_t *fw = sentry__filewriter_new(filepath);
        TEST_ASSERT(!!fw);
        for (int j = 0; j < 500; j++) {
            sentry__filewriter_write(fw, "0123456789", 10);
        }
        TEST_CHECK_INT_EQUAL(sentry__filewriter_byte_count(fw), 5000);
        sentry__filewriter_free(fw);
        sentry__path_free(filepath);
    }

    sentry__cleanup_cache(options);

    int cache_count = 0;
    size_t cache_size = 0;
    sentry_pathiter_t *iter = sentry__path_iter_directory(cache_path);
    const sentry_path_t *entry;
    while (iter && (entry = sentry__pathiter_next(iter)) != NULL) {
        cache_count++;
        cache_size += sentry__path_get_size(entry);
    }
    sentry__pathiter_free(iter);

    TEST_CHECK_INT_EQUAL(cache_count, 2);
    TEST_CHECK(cache_size <= 10 * 1024);
    TEST_CHECK(!sentry__path_is_file(retry_path));
    TEST_CHECK(!sentry__path_is_file(sib_path));

    sentry__path_free(retry_path);
    sentry__path_free(sib_path);
    sentry__path_free(cache_path);
    sentry_close();
}

SENTRY_TEST(cache_max_age)
{
#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#endif
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_cache_keep(options, true);
    sentry_options_set_cache_max_age(options, 5 * 24 * 60 * 60); // 5 days
    sentry_init(options);

    sentry_path_t *cache_path
        = sentry__path_join_str(options->database_path, "cache");
    TEST_ASSERT(!!cache_path);
    TEST_ASSERT(sentry__path_remove_all(cache_path) == 0);
    TEST_ASSERT(sentry__path_create_dir_all(cache_path) == 0);

    // 0,2,4,6,8 days ago
    time_t now = time(NULL);
    for (int i = 0; i < 5; i++) {
        sentry_uuid_t event_id = sentry_uuid_new_v4();
        char *filename = sentry__uuid_as_filename(&event_id, ".envelope");
        TEST_ASSERT(!!filename);
        sentry_path_t *filepath = sentry__path_join_str(cache_path, filename);
        sentry_free(filename);

        TEST_ASSERT(sentry__path_touch(filepath) == 0);
        time_t mtime = now - (i * 2 * 24 * 60 * 60); // N days ago
        TEST_ASSERT(set_file_mtime(filepath, mtime) == 0);
        sentry__path_free(filepath);
    }

    sentry__cleanup_cache(options);

    int cache_count = 0;
    sentry_pathiter_t *iter = sentry__path_iter_directory(cache_path);
    const sentry_path_t *entry;
    while (iter && (entry = sentry__pathiter_next(iter)) != NULL) {
        cache_count++;
        time_t mtime = sentry__path_get_mtime(entry);
        TEST_CHECK(now - mtime <= (5 * 24 * 60 * 60));
    }
    sentry__pathiter_free(iter);

    TEST_CHECK_INT_EQUAL(cache_count, 3);

    sentry__path_free(cache_path);
    sentry_close();
}

SENTRY_TEST(cache_max_items)
{
#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#endif
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_cache_keep(options, true);
    sentry_options_set_cache_max_items(options, 5);
    sentry_init(options);

    sentry_path_t *cache_path
        = sentry__path_join_str(options->database_path, "cache");
    TEST_ASSERT(!!cache_path);
    TEST_ASSERT(sentry__path_remove_all(cache_path) == 0);
    TEST_ASSERT(sentry__path_create_dir_all(cache_path) == 0);

    time_t now = time(NULL);
    for (int i = 0; i < 10; i++) {
        sentry_uuid_t event_id = sentry_uuid_new_v4();
        char *filename = sentry__uuid_as_filename(&event_id, ".envelope");
        TEST_ASSERT(!!filename);
        sentry_path_t *filepath = sentry__path_join_str(cache_path, filename);
        sentry_free(filename);

        TEST_ASSERT(sentry__path_touch(filepath) == 0);
        time_t mtime = now - (i * 60);
        TEST_ASSERT(set_file_mtime(filepath, mtime) == 0);
        sentry__path_free(filepath);
    }

    sentry__cleanup_cache(options);

    int cache_count = 0;
    sentry_pathiter_t *iter = sentry__path_iter_directory(cache_path);
    const sentry_path_t *entry;
    while (iter && (entry = sentry__pathiter_next(iter)) != NULL) {
        cache_count++;
    }
    sentry__pathiter_free(iter);

    TEST_CHECK_INT_EQUAL(cache_count, 5);

    sentry__path_free(cache_path);
    sentry_close();
}

SENTRY_TEST(cache_max_items_with_retry)
{
#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#endif
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_cache_keep(options, true);
    sentry_options_set_cache_max_items(options, 7);
    sentry_init(options);

    sentry_path_t *cache_path
        = sentry__path_join_str(options->database_path, "cache");
    TEST_ASSERT(!!cache_path);
    TEST_ASSERT(sentry__path_remove_all(cache_path) == 0);
    TEST_ASSERT(sentry__path_create_dir_all(cache_path) == 0);

    time_t now = time(NULL);

    // 5 cache-format files: 1,3,5,7,9 min old
    for (int i = 0; i < 5; i++) {
        sentry_uuid_t event_id = sentry_uuid_new_v4();
        char *filename = sentry__uuid_as_filename(&event_id, ".envelope");
        TEST_ASSERT(!!filename);
        sentry_path_t *filepath = sentry__path_join_str(cache_path, filename);
        sentry_free(filename);

        TEST_ASSERT(sentry__path_touch(filepath) == 0);
        TEST_ASSERT(set_file_mtime(filepath, now - ((i * 2 + 1) * 60)) == 0);
        sentry__path_free(filepath);
    }

    // 5 retry-format files: 0,2,4,6,8 min old
    for (int i = 0; i < 5; i++) {
        sentry_uuid_t event_id = sentry_uuid_new_v4();
        char uuid[37];
        sentry_uuid_as_string(&event_id, uuid);
        char filename[128];
        snprintf(filename, sizeof(filename), "%" PRIu64 "-00-%.36s.envelope",
            (uint64_t)now, uuid);
        sentry_path_t *filepath = sentry__path_join_str(cache_path, filename);

        TEST_ASSERT(sentry__path_touch(filepath) == 0);
        TEST_ASSERT(set_file_mtime(filepath, now - (i * 2 * 60)) == 0);
        sentry__path_free(filepath);
    }

    sentry__cleanup_cache(options);

    int total_count = 0;
    sentry_pathiter_t *iter = sentry__path_iter_directory(cache_path);
    const sentry_path_t *entry;
    while (iter && (entry = sentry__pathiter_next(iter)) != NULL) {
        total_count++;
    }
    sentry__pathiter_free(iter);

    TEST_CHECK_INT_EQUAL(total_count, 7);

    sentry__path_free(cache_path);
    sentry_close();
}

SENTRY_TEST(cache_remove_siblings)
{
#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#endif
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_init(options);

    sentry_path_t *cache_path
        = sentry__path_join_str(options->database_path, "cache");
    TEST_ASSERT(!!cache_path);
    TEST_ASSERT(sentry__path_remove_all(cache_path) == 0);
    TEST_ASSERT(sentry__path_create_dir_all(cache_path) == 0);

    sentry_path_t *retry_env_path = sentry__path_join_str(cache_path,
        "1234567890-01-c993afb6-b4ac-48a6-b61b-2558e601d65d.envelope");
    sentry_path_t *bare_env_path = sentry__path_join_str(
        cache_path, "c993afb6-b4ac-48a6-b61b-2558e601d65d.envelope");
    sentry_path_t *dmp_path = sentry__path_join_str(
        cache_path, "c993afb6-b4ac-48a6-b61b-2558e601d65d.dmp");
    sentry_path_t *sibling_path = sentry__path_join_str(
        cache_path, "c993afb6-b4ac-48a6-b61b-2558e601d65d-attachment.bin");
    sentry_path_t *unrelated_path = sentry__path_join_str(
        cache_path, "c993afb6-b4ac-48a6-b61b-2558e601d65d_attachment.bin");
    TEST_ASSERT(!!retry_env_path);
    TEST_ASSERT(!!bare_env_path);
    TEST_ASSERT(!!dmp_path);
    TEST_ASSERT(!!sibling_path);
    TEST_ASSERT(!!unrelated_path);

    TEST_ASSERT(sentry__path_write_buffer(retry_env_path, "retry", 5) == 0);
    TEST_ASSERT(sentry__path_write_buffer(bare_env_path, "cached", 6) == 0);
    TEST_ASSERT(sentry__path_write_buffer(dmp_path, "dmp", 3) == 0);
    TEST_ASSERT(sentry__path_write_buffer(sibling_path, "attachment", 10) == 0);
    TEST_ASSERT(sentry__path_write_buffer(unrelated_path, "keep", 4) == 0);

    sentry__cache_remove_envelope(retry_env_path);

    TEST_CHECK(!sentry__path_is_file(retry_env_path));
    TEST_CHECK(sentry__path_is_file(bare_env_path));
    TEST_CHECK(!sentry__path_is_file(dmp_path));
    TEST_CHECK(!sentry__path_is_file(sibling_path));
    TEST_CHECK(sentry__path_is_file(unrelated_path));

    sentry__path_remove(bare_env_path);
    sentry__path_remove(unrelated_path);
    sentry__path_free(retry_env_path);
    sentry__path_free(bare_env_path);
    sentry__path_free(dmp_path);
    sentry__path_free(sibling_path);
    sentry__path_free(unrelated_path);
    sentry__path_free(cache_path);
    sentry_close();
}

SENTRY_TEST(cache_prune_siblings)
{
#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#endif
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_cache_max_items(options, 1);
    sentry_init(options);

    sentry_path_t *cache_path
        = sentry__path_join_str(options->database_path, "cache");
    TEST_ASSERT(!!cache_path);
    TEST_ASSERT(sentry__path_remove_all(cache_path) == 0);
    TEST_ASSERT(sentry__path_create_dir_all(cache_path) == 0);

    time_t now = time(NULL);
    sentry_path_t *new_env = sentry__path_join_str(
        cache_path, "c993afb6-b4ac-48a6-b61b-2558e601d65d.envelope");
    sentry_path_t *old_env = sentry__path_join_str(
        cache_path, "97e8cc2b-94f6-42ef-ae56-bc67015e7f22.envelope");
    sentry_path_t *old_dmp = sentry__path_join_str(
        cache_path, "97e8cc2b-94f6-42ef-ae56-bc67015e7f22.dmp");
    sentry_path_t *old_sibling = sentry__path_join_str(
        cache_path, "97e8cc2b-94f6-42ef-ae56-bc67015e7f22-attachment.bin");
    TEST_ASSERT(!!new_env);
    TEST_ASSERT(!!old_env);
    TEST_ASSERT(!!old_dmp);
    TEST_ASSERT(!!old_sibling);

    TEST_ASSERT(sentry__path_touch(new_env) == 0);
    TEST_ASSERT(sentry__path_touch(old_env) == 0);
    TEST_ASSERT(sentry__path_write_buffer(old_dmp, "dmp", 3) == 0);
    TEST_ASSERT(sentry__path_write_buffer(old_sibling, "attachment", 10) == 0);
    TEST_ASSERT(set_file_mtime(new_env, now) == 0);
    TEST_ASSERT(set_file_mtime(old_env, now - 60) == 0);

    sentry__cleanup_cache(options);

    TEST_CHECK(sentry__path_is_file(new_env));
    TEST_CHECK(!sentry__path_is_file(old_env));
    TEST_CHECK(!sentry__path_is_file(old_dmp));
    TEST_CHECK(!sentry__path_is_file(old_sibling));

    sentry__path_free(new_env);
    sentry__path_free(old_env);
    sentry__path_free(old_dmp);
    sentry__path_free(old_sibling);
    sentry__path_free(cache_path);
    sentry_close();
}

SENTRY_TEST(cache_symlink_run)
{
#if !defined(SENTRY_PLATFORM_UNIX) || defined(SENTRY_PLATFORM_NX)              \
    || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#else
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_init(options);

    sentry_path_t *target_dir
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".old-run-target");
    TEST_ASSERT(!!target_dir);
    sentry_path_t *target_file = sentry__path_join_str(target_dir, "important");
    TEST_ASSERT(!!target_file);
    sentry_path_t *linked_run_dir
        = sentry__path_join_str(options->database_path, "malicious.run");
    TEST_ASSERT(!!linked_run_dir);
    sentry_path_t *linked_run_lock
        = sentry__path_append_str(linked_run_dir, ".lock");
    TEST_ASSERT(!!linked_run_lock);

    // Model an old run entry like:
    //   <database>/malicious.run -> <cwd>/.old-run-target
    // The target stays outside the database so following the link would remove
    // files outside SDK-owned cache directories.
    sentry__path_remove(linked_run_dir);
    sentry__path_remove(linked_run_lock);
    TEST_ASSERT(sentry__path_remove_all(target_dir) == 0);
    TEST_ASSERT(sentry__path_create_dir_all(target_dir) == 0);
    TEST_ASSERT(sentry__path_write_buffer(target_file, "keep", 4) == 0);
    sentry_path_t *target_dir_abs = sentry__path_absolute(target_dir);
    TEST_ASSERT(!!target_dir_abs);
    TEST_ASSERT(symlink(target_dir_abs->path, linked_run_dir->path) == 0);

    sentry__process_old_runs(options, 0);

    TEST_CHECK(sentry__path_is_file(target_file));

    sentry__path_remove(linked_run_dir);
    sentry__path_remove(linked_run_lock);
    sentry__path_remove_all(target_dir);
    sentry__path_free(linked_run_lock);
    sentry__path_free(linked_run_dir);
    sentry__path_free(target_dir_abs);
    sentry__path_free(target_file);
    sentry__path_free(target_dir);
    sentry_close();
#endif
}

SENTRY_TEST(cache_consent_revoked)
{
#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#endif
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_cache_keep(options, true);
    sentry_options_set_require_user_consent(options, true);
    sentry_init(options);
    sentry_user_consent_revoke();

    sentry_path_t *cache_path
        = sentry__path_join_str(options->database_path, "cache");
    TEST_ASSERT(!!cache_path);
    sentry__path_remove_all(cache_path);

    sentry_capture_event(
        sentry_value_new_message_event(SENTRY_LEVEL_INFO, "test", "revoked"));

    int count = 0;
    bool is_retry_format = false;
    sentry_pathiter_t *iter = sentry__path_iter_directory(cache_path);
    const sentry_path_t *entry;
    while (iter && (entry = sentry__pathiter_next(iter)) != NULL) {
        if (sentry__path_ends_with(entry, ".envelope")) {
            count++;
            uint64_t ts;
            int attempt;
            const char *uuid;
            is_retry_format = sentry__parse_cache_filename(
                sentry__path_filename(entry), &ts, &attempt, &uuid);
        }
    }
    sentry__pathiter_free(iter);
    TEST_CHECK_INT_EQUAL(count, 1);
    TEST_CHECK(is_retry_format);

    sentry__path_free(cache_path);
    sentry_close();
}

SENTRY_TEST(cache_consent_revoked_nocache)
{
#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#endif
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_cache_keep(options, false);
    sentry_options_set_require_user_consent(options, true);
    sentry_init(options);
    sentry_user_consent_revoke();

    sentry_path_t *cache_path
        = sentry__path_join_str(options->database_path, "cache");
    TEST_ASSERT(!!cache_path);
    sentry__path_remove_all(cache_path);

    sentry_capture_event(
        sentry_value_new_message_event(SENTRY_LEVEL_INFO, "test", "revoked"));

    int count = 0;
    sentry_pathiter_t *iter = sentry__path_iter_directory(cache_path);
    const sentry_path_t *entry;
    while (iter && (entry = sentry__pathiter_next(iter)) != NULL) {
        if (sentry__path_ends_with(entry, ".envelope")) {
            count++;
        }
    }
    sentry__pathiter_free(iter);
    TEST_CHECK_INT_EQUAL(count, 0);

    sentry__path_free(cache_path);
    sentry_close();
}

SENTRY_TEST(cache_max_size_and_age)
{
#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#endif
    // Verify size pruning keeps newer entries, removes all older once limit
    // hit. A (5KB), B (6KB), C (3KB) newest-to-oldest, max_size=10KB A+B=11KB
    // exceeds limit -> B pruned, C (older) also pruned
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_cache_keep(options, true);
    sentry_options_set_cache_max_size(options, 10 * 1024); // 10 KB
    sentry_init(options);

    sentry_path_t *cache_path
        = sentry__path_join_str(options->database_path, "cache");
    TEST_ASSERT(!!cache_path);
    TEST_ASSERT(sentry__path_remove_all(cache_path) == 0);
    TEST_ASSERT(sentry__path_create_dir_all(cache_path) == 0);

    time_t now = time(NULL);

    // A (newest, 5KB), B (middle, 6KB), C (oldest, 3KB)
    struct {
        const char *name;
        size_t size;
        time_t age;
    } files[] = {
        { "a.envelope", 5 * 1024, 0 }, // newest
        { "b.envelope", 6 * 1024, 60 }, // 1 min old
        { "c.envelope", 3 * 1024, 120 }, // 2 min old
    };

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        sentry_path_t *filepath
            = sentry__path_join_str(cache_path, files[i].name);
        TEST_ASSERT(!!filepath);

        sentry_filewriter_t *fw = sentry__filewriter_new(filepath);
        TEST_ASSERT(!!fw);
        for (size_t j = 0; j < files[i].size / 10; j++) {
            sentry__filewriter_write(fw, "0123456789", 10);
        }
        sentry__filewriter_free(fw);

        TEST_ASSERT(set_file_mtime(filepath, now - files[i].age) == 0);
        sentry__path_free(filepath);
    }

    sentry__cleanup_cache(options);

    // Verify: A kept, B and C removed (once limit hit, all older removed)
    sentry_path_t *a_path = sentry__path_join_str(cache_path, "a.envelope");
    sentry_path_t *b_path = sentry__path_join_str(cache_path, "b.envelope");
    sentry_path_t *c_path = sentry__path_join_str(cache_path, "c.envelope");

    TEST_CHECK(sentry__path_is_file(a_path)); // newest, kept
    TEST_CHECK(!sentry__path_is_file(b_path)); // size-pruned
    TEST_CHECK(!sentry__path_is_file(c_path)); // older than B, also pruned

    sentry__path_free(a_path);
    sentry__path_free(b_path);
    sentry__path_free(c_path);
    sentry__path_free(cache_path);
    sentry_close();
}

SENTRY_TEST(cache_write_minidump)
{
#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#endif
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_cache_keep(options, true);
    sentry_init(options);

    sentry_path_t *cache_path
        = sentry__path_join_str(options->database_path, "cache");
    TEST_ASSERT(!!cache_path);
    TEST_ASSERT(sentry__path_remove_all(cache_path) == 0);

    sentry_uuid_t event_id = sentry_uuid_new_v4();
    sentry_envelope_t *envelope = sentry__envelope_new();
    TEST_ASSERT(!!envelope);

    sentry_value_t event = sentry__value_new_event_with_id(&event_id);
    sentry__envelope_add_event(envelope, event);

    const char *minidump_data = "fake_minidump_payload";
    size_t minidump_len = strlen(minidump_data);
    sentry_envelope_item_t *item = sentry__envelope_add_from_buffer(
        envelope, minidump_data, minidump_len, "attachment");
    TEST_ASSERT(!!item);
    sentry__envelope_item_set_header(
        item, "attachment_type", sentry_value_new_string("event.minidump"));
    sentry__envelope_item_set_header(
        item, "filename", sentry_value_new_string("minidump.dmp"));

    TEST_CHECK(sentry__run_write_cache(options->run, envelope, -1));
    sentry_envelope_free(envelope);

    char *env_filename = sentry__uuid_as_filename(&event_id, ".envelope");
    char *dmp_filename = sentry__uuid_as_filename(&event_id, ".dmp");
    TEST_ASSERT(!!env_filename);
    TEST_ASSERT(!!dmp_filename);

    sentry_path_t *env_path = sentry__path_join_str(cache_path, env_filename);
    sentry_path_t *dmp_path = sentry__path_join_str(cache_path, dmp_filename);

    // both files should exist
    TEST_CHECK(sentry__path_is_file(env_path));
    TEST_CHECK(sentry__path_is_file(dmp_path));

    // .dmp should contain the minidump payload
    size_t dmp_buf_len = 0;
    char *dmp_buf = sentry__path_read_to_buffer(dmp_path, &dmp_buf_len);
    TEST_ASSERT(!!dmp_buf);
    TEST_CHECK_INT_EQUAL(dmp_buf_len, minidump_len);
    TEST_CHECK(memcmp(dmp_buf, minidump_data, minidump_len) == 0);
    sentry_free(dmp_buf);

    // .envelope should NOT contain the minidump
    size_t env_buf_len = 0;
    char *env_buf = sentry__path_read_to_buffer(env_path, &env_buf_len);
    TEST_ASSERT(!!env_buf);
    sentry_envelope_t *cached
        = sentry_envelope_deserialize(env_buf, env_buf_len);
    sentry_free(env_buf);
    TEST_ASSERT(!!cached);
    TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(cached), 1);
    const sentry_envelope_item_t *cached_item
        = sentry__envelope_get_item(cached, 0);
    TEST_ASSERT(!!cached_item);
    const char *type = sentry_value_as_string(
        sentry__envelope_item_get_header(cached_item, "type"));
    TEST_CHECK(sentry__string_eq(type, "event"));
    sentry_envelope_free(cached);

    sentry__path_free(env_path);
    sentry__path_free(dmp_path);
    sentry_free(env_filename);
    sentry_free(dmp_filename);
    sentry__path_free(cache_path);
    sentry_close();
}

SENTRY_TEST(cache_write_raw_with_minidump)
{
#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#endif
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_cache_keep(options, true);
    sentry_init(options);

    sentry_path_t *cache_path
        = sentry__path_join_str(options->database_path, "cache");
    TEST_ASSERT(!!cache_path);
    TEST_ASSERT(sentry__path_remove_all(cache_path) == 0);

    // build a structured envelope, serialize it, then reload as raw
    sentry_uuid_t event_id = sentry_uuid_new_v4();
    sentry_envelope_t *envelope = sentry__envelope_new();
    TEST_ASSERT(!!envelope);

    sentry_value_t event = sentry__value_new_event_with_id(&event_id);
    sentry__envelope_add_event(envelope, event);

    const char *minidump_data = "raw_minidump_payload";
    size_t minidump_len = strlen(minidump_data);
    sentry_envelope_item_t *item = sentry__envelope_add_from_buffer(
        envelope, minidump_data, minidump_len, "attachment");
    TEST_ASSERT(!!item);
    sentry__envelope_item_set_header(
        item, "attachment_type", sentry_value_new_string("event.minidump"));

    // serialize to a temp file and reload as raw
    sentry_path_t *tmp_path = sentry__path_join_str(cache_path, "tmp.envelope");
    TEST_ASSERT(sentry__path_create_dir_all(cache_path) == 0);
    TEST_ASSERT(sentry_envelope_write_to_path(envelope, tmp_path) == 0);
    sentry_envelope_free(envelope);

    sentry_envelope_t *raw = sentry__envelope_from_path(tmp_path);
    sentry__path_remove(tmp_path);
    sentry__path_free(tmp_path);
    TEST_ASSERT(!!raw);

    // write raw envelope to cache — should materialize and split
    TEST_CHECK(sentry__run_write_cache(options->run, raw, -1));
    sentry_envelope_free(raw);

    char *dmp_filename = sentry__uuid_as_filename(&event_id, ".dmp");
    TEST_ASSERT(!!dmp_filename);
    sentry_path_t *dmp_path = sentry__path_join_str(cache_path, dmp_filename);
    TEST_CHECK(sentry__path_is_file(dmp_path));

    size_t dmp_buf_len = 0;
    char *dmp_buf = sentry__path_read_to_buffer(dmp_path, &dmp_buf_len);
    TEST_ASSERT(!!dmp_buf);
    TEST_CHECK_INT_EQUAL(dmp_buf_len, minidump_len);
    TEST_CHECK(memcmp(dmp_buf, minidump_data, minidump_len) == 0);
    sentry_free(dmp_buf);

    sentry__path_free(dmp_path);
    sentry_free(dmp_filename);
    sentry__path_free(cache_path);
    sentry_close();
}
