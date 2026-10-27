#include "sentry_path.h"
#include "sentry_string.h"
#include "sentry_testsupport.h"

#if defined(SENTRY_PLATFORM_UNIX) && !defined(SENTRY_PLATFORM_NX)              \
    && !defined(SENTRY_PLATFORM_PS)
#    include <unistd.h>
#endif

SENTRY_TEST(recursive_paths)
{
    sentry_path_t *base = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".foo");
    TEST_ASSERT(!!base);
    sentry_path_t *nested = sentry__path_join_str(base, "bar");
    TEST_ASSERT(!!nested);
    sentry_path_t *nested2 = sentry__path_join_str(nested, "baz");
    TEST_ASSERT(!!nested2);
    sentry_path_t *file =
#ifdef SENTRY_PLATFORM_WINDOWS
        sentry__path_join_wstr(nested2, L"unicode ❤️ Юля.txt");
#else
        sentry__path_join_str(nested2, "unicode ❤️ Юля.txt");
#endif
    TEST_ASSERT(!!file);

    sentry__path_create_dir_all(nested2);
    sentry__path_touch(file);

    TEST_CHECK(sentry__path_is_file(file));

    sentry__path_remove_all(nested);

    TEST_CHECK(!sentry__path_is_file(file));
    TEST_CHECK(!sentry__path_is_file(nested));
    TEST_CHECK(sentry__path_is_dir(base));

    sentry__path_remove_all(base);

    sentry__path_free(file);
    sentry__path_free(nested2);
    sentry__path_free(nested);
    sentry__path_free(base);
}

SENTRY_TEST(path_joining_unix)
{
#ifndef SENTRY_PLATFORM_UNIX
    SKIP_TEST();
#else
    sentry_path_t *path = sentry__path_new("foo/bar/baz.txt");
    TEST_ASSERT(!!path);
    sentry_path_t *joined;

    TEST_CHECK(strcmp(path->path, "foo/bar/baz.txt") == 0);
    TEST_CHECK(strcmp(sentry__path_filename(path), "baz.txt") == 0);

    joined = sentry__path_join_str(path, "extra");
    TEST_ASSERT(!!joined);
    TEST_CHECK(strcmp(joined->path, "foo/bar/baz.txt/extra") == 0);
    TEST_CHECK(strcmp(sentry__path_filename(joined), "extra") == 0);
    sentry__path_free(joined);

    joined = sentry__path_join_str(path, "/root/path");
    TEST_ASSERT(!!joined);
    TEST_CHECK(strcmp(joined->path, "/root/path") == 0);
    TEST_CHECK(strcmp(sentry__path_filename(joined), "path") == 0);
    sentry__path_free(joined);

    sentry__path_free(path);
#endif
}

SENTRY_TEST(path_from_str_null)
{
    TEST_CHECK(NULL == sentry__path_from_str(NULL));
    TEST_CHECK(NULL == sentry__path_from_str_n(NULL, 0));
    TEST_CHECK(NULL == sentry__path_from_str_n(NULL, 10));
}

SENTRY_TEST(path_from_str_n_wo_null_termination)
{
    // provide non-null-terminated path string with buffer character at the end.
    char path_str[] = { 't', 'e', 's', 't', 'X' };
    sentry_path_t *test_path = sentry__path_from_str_n(path_str, 4);
    TEST_ASSERT(!!test_path);
    TEST_CHECK_STRING_EQUAL(test_path->path, "test");
    sentry__path_free(test_path);
}

SENTRY_TEST(path_joining_windows)
{
#ifndef SENTRY_PLATFORM_WINDOWS
    SKIP_TEST();
#else
    sentry_path_t *path = sentry__path_new("foo/bar/baz.txt");
    sentry_path_t *winpath = sentry__path_new("foo\\bar\\baz.txt");
    sentry_path_t *awinpath = sentry__path_from_str("foo\\bar\\baz.txt");
    sentry_path_t *cpath = sentry__path_from_str("C:\\foo\\bar\\baz.txt");
    TEST_ASSERT(!!path);
    TEST_ASSERT(!!winpath);
    TEST_ASSERT(!!awinpath);
    TEST_ASSERT(!!cpath);

    TEST_CHECK(_stricmp(path->path, "foo/bar/baz.txt") == 0);
    TEST_CHECK(_stricmp(sentry__path_filename(path), "baz.txt") == 0);
    TEST_CHECK(_stricmp(winpath->path, "foo\\bar\\baz.txt") == 0);
    TEST_CHECK(_stricmp(awinpath->path, "foo\\bar\\baz.txt") == 0);
    TEST_CHECK(_stricmp(sentry__path_filename(winpath), "baz.txt") == 0);

    TEST_CHECK(_wcsicmp(path->path_w, L"foo/bar/baz.txt") == 0);
    TEST_CHECK(_wcsicmp(sentry__path_filename_w(path), L"baz.txt") == 0);
    TEST_CHECK(_wcsicmp(winpath->path_w, L"foo\\bar\\baz.txt") == 0);
    TEST_CHECK(_wcsicmp(awinpath->path_w, L"foo\\bar\\baz.txt") == 0);
    TEST_CHECK(_wcsicmp(sentry__path_filename_w(winpath), L"baz.txt") == 0);

    sentry_path_t *joined = sentry__path_join_str(path, "extra");
    TEST_CHECK(_stricmp(joined->path, "foo/bar/baz.txt\\extra") == 0);
    TEST_CHECK(_stricmp(sentry__path_filename(joined), "extra") == 0);
    TEST_CHECK(_wcsicmp(joined->path_w, L"foo/bar/baz.txt\\extra") == 0);
    TEST_CHECK(_wcsicmp(sentry__path_filename_w(joined), L"extra") == 0);
    sentry__path_free(joined);

    joined = sentry__path_join_str(path, "/root/path");
    TEST_CHECK(_stricmp(joined->path, "/root/path") == 0);
    TEST_CHECK(_stricmp(sentry__path_filename(joined), "path") == 0);
    TEST_CHECK(_wcsicmp(joined->path_w, L"/root/path") == 0);
    TEST_CHECK(_wcsicmp(sentry__path_filename_w(joined), L"path") == 0);
    sentry__path_free(joined);

    joined = sentry__path_join_str(cpath, "/root/path");
    TEST_CHECK(_stricmp(joined->path, "C:/root/path") == 0);
    TEST_CHECK(_wcsicmp(joined->path_w, L"C:/root/path") == 0);
    sentry__path_free(joined);

    joined = sentry__path_join_str(cpath, "D:\\root\\path");
    TEST_CHECK(_stricmp(joined->path, "D:\\root\\path") == 0);
    TEST_CHECK(_wcsicmp(joined->path_w, L"D:\\root\\path") == 0);
    sentry__path_free(joined);

    joined = sentry__path_join_str(cpath, "\\root\\path");
    TEST_CHECK(_stricmp(joined->path, "C:\\root\\path") == 0);
    TEST_CHECK(_wcsicmp(joined->path_w, L"C:\\root\\path") == 0);
    sentry__path_free(joined);

    sentry__path_free(cpath);
    sentry__path_free(awinpath);
    sentry__path_free(winpath);
    sentry__path_free(path);
#endif
}

SENTRY_TEST(path_relative_filename)
{
    sentry_path_t *path = sentry__path_from_str("foobar.txt");
    TEST_ASSERT(!!path);
    TEST_CHECK_STRING_EQUAL(sentry__path_filename(path), "foobar.txt");
    sentry__path_free(path);
}

SENTRY_TEST(path_basics)
{
    size_t items = 0;
    const sentry_path_t *p;
    sentry_path_t *path = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".");
    TEST_CHECK(!!path);

    sentry_pathiter_t *piter = sentry__path_iter_directory(path);
    TEST_ASSERT(piter != NULL);
    while ((p = sentry__pathiter_next(piter)) != NULL) {
        bool is_file = sentry__path_is_file(p);
        bool is_dir = sentry__path_is_dir(p);
        TEST_CHECK(is_file || is_dir);
        items += 1;
    }
    TEST_CHECK(items > 0);

    sentry__pathiter_free(piter);
    sentry__path_free(path);
}

SENTRY_TEST(path_current_exe)
{
    sentry_path_t *path = sentry__path_current_exe();
#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_PS)
    // Not available on NX or PS
    TEST_CHECK(!path);
#else
    TEST_CHECK(!!path);
    if (path) {
        TEST_CHECK(sentry__path_is_file(path));
        sentry__path_free(path);
    }
#endif
}

SENTRY_TEST(path_directory)
{
    sentry_path_t *path_1
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX "foo");
    TEST_ASSERT(!!path_1);
    sentry_path_t *path_2
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX "foo/bar");
    TEST_ASSERT(!!path_2);
#ifdef SENTRY_PLATFORM_WINDOWS
    sentry_path_t *path_3
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX "foo/bar\\baz");

    // `%TEMP%\sentry_test_unit`
    wchar_t temp_folder[MAX_PATH];
    GetEnvironmentVariableW(L"TEMP", temp_folder, sizeof(temp_folder));
    sentry_path_t *path_4 = sentry__path_from_wstr(temp_folder);
    path_4 = sentry__path_join_str(path_4, "sentry_test_unit");
#endif

    // cleanup before tests
    sentry__path_remove_all(path_1);

    // create single directory
    sentry__path_create_dir_all(path_1);
    TEST_CHECK(sentry__path_is_dir(path_1));

    sentry__path_remove(path_1);
    TEST_CHECK(!sentry__path_is_dir(path_1));

    // create directories by path with forward slash
    sentry__path_create_dir_all(path_2);
    TEST_CHECK(sentry__path_is_dir(path_2));

    sentry__path_remove_all(path_2);
    TEST_CHECK(!sentry__path_is_dir(path_2));

#ifdef SENTRY_PLATFORM_WINDOWS
    // create directories by path with forward slash and backward slashes
    sentry__path_create_dir_all(path_3);
    TEST_CHECK(sentry__path_is_dir(path_3));

    sentry__path_remove_all(path_3);
    TEST_CHECK(!sentry__path_is_dir(path_3));
    sentry__path_free(path_3);

    // create directories with absolute path
    sentry__path_create_dir_all(path_4);
    TEST_CHECK(sentry__path_is_dir(path_4));

    sentry__path_remove_all(path_4);
    TEST_CHECK(!sentry__path_is_dir(path_4));
    sentry__path_free(path_4);
#endif

    sentry__path_remove_all(path_1);
    sentry__path_free(path_1);
    sentry__path_free(path_2);
}

SENTRY_TEST(path_remove_all_symlink)
{
#if !defined(SENTRY_PLATFORM_UNIX) || defined(SENTRY_PLATFORM_NX)              \
    || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#else
    sentry_path_t *base
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".remove-all");
    TEST_ASSERT(!!base);
    sentry_path_t *target = sentry__path_join_str(base, "target");
    TEST_ASSERT(!!target);
    sentry_path_t *target_file = sentry__path_join_str(target, "important");
    TEST_ASSERT(!!target_file);
    sentry_path_t *link = sentry__path_join_str(base, "link");
    TEST_ASSERT(!!link);

    // Model a recursive deletion target like:
    //   .remove-all/link -> .remove-all/target
    // Removing the link must not recurse into the target directory.
    TEST_ASSERT(sentry__path_remove_all(base) == 0);
    TEST_ASSERT(sentry__path_create_dir_all(target) == 0);
    TEST_ASSERT(sentry__path_write_buffer(target_file, "keep", 4) == 0);
    sentry_path_t *target_abs = sentry__path_absolute(target);
    TEST_ASSERT(!!target_abs);
    TEST_ASSERT(symlink(target_abs->path, link->path) == 0);
    TEST_CHECK(sentry__path_is_symlink(link));

    TEST_ASSERT(sentry__path_remove_all(link) == 0);

    TEST_CHECK(sentry__path_is_file(target_file));
    TEST_CHECK(!sentry__path_is_dir(link));

    sentry__path_remove_all(base);
    sentry__path_free(link);
    sentry__path_free(target_file);
    sentry__path_free(target_abs);
    sentry__path_free(target);
    sentry__path_free(base);
#endif
}

SENTRY_TEST(path_mtime)
{
#if defined(SENTRY_PLATFORM_NX)
    SKIP_TEST();
#endif

    sentry_path_t *path
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX "foo.txt");
    TEST_ASSERT(!!path);

    TEST_CHECK(sentry__path_remove(path) == 0);
    TEST_CHECK(!sentry__path_is_file(path));
    TEST_CHECK(sentry__path_get_mtime(path) <= 0);

    sentry__path_touch(path);
    TEST_CHECK(sentry__path_is_file(path));

    const time_t before = sentry__path_get_mtime(path);
    TEST_CHECK(before > 0);

    sleep_s(1);

    sentry__path_write_buffer(path, "after", 5);
    const time_t after = sentry__path_get_mtime(path);
    TEST_CHECK(after > 0);
    TEST_CHECK(before < after);

    sentry__path_remove(path);
    sentry__path_free(path);
}

SENTRY_TEST(path_rename)
{
#if defined(SENTRY_PLATFORM_NX)
    SKIP_TEST();
#endif
    sentry_path_t *src
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".rename-src");
    TEST_ASSERT(!!src);
    sentry_path_t *dst
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".rename-dst");
    TEST_ASSERT(!!dst);

    // cleanup
    sentry__path_remove_all(src);
    sentry__path_remove_all(dst);

    // rename file
    sentry__path_touch(src);
    TEST_CHECK(sentry__path_is_file(src));
    TEST_CHECK(sentry__path_rename(src, dst) == 0);
    TEST_CHECK(!sentry__path_is_file(src));
    TEST_CHECK(sentry__path_is_file(dst));
    sentry__path_remove(dst);

    // rename with content preserved
    sentry__path_write_buffer(src, "hello", 5);
    TEST_CHECK(sentry__path_rename(src, dst) == 0);
    size_t len = 0;
    char *buf = sentry__path_read_to_buffer(dst, &len);
    TEST_ASSERT(!!buf);
    TEST_CHECK(len == 5);
    TEST_CHECK(memcmp(buf, "hello", 5) == 0);
    sentry_free(buf);
    sentry__path_remove(dst);

    // overwrite existing dst
    sentry__path_write_buffer(src, "src-data", 8);
    sentry__path_write_buffer(dst, "dst-data", 8);
    TEST_CHECK(sentry__path_rename(src, dst) == 0);
    TEST_CHECK(!sentry__path_is_file(src));
    buf = sentry__path_read_to_buffer(dst, &len);
    TEST_ASSERT(!!buf);
    TEST_CHECK(len == 8);
    TEST_CHECK(memcmp(buf, "src-data", 8) == 0);
    sentry_free(buf);
    sentry__path_remove(dst);

    // rename nonexistent src
    TEST_CHECK(sentry__path_rename(src, dst) != 0);

    // rename directory
    sentry__path_create_dir_all(src);
    TEST_CHECK(sentry__path_is_dir(src));
    TEST_CHECK(sentry__path_rename(src, dst) == 0);
    TEST_CHECK(!sentry__path_is_dir(src));
    TEST_CHECK(sentry__path_is_dir(dst));
    sentry__path_remove_all(dst);

    sentry__path_free(dst);
    sentry__path_free(src);
}

SENTRY_TEST(path_basename)
{
    sentry_path_t *path;
    sentry_path_t *base;

    // strips matching suffix
    path = sentry__path_from_str("/foo/bar.envelope");
    base = sentry__path_basename(path, ".envelope");
    TEST_CHECK_STRING_EQUAL(base->path, "/foo/bar");
    sentry__path_free(base);
    sentry__path_free(path);

    // strips compound suffix
    path = sentry__path_from_str("/foo/bar.tar.gz");
    base = sentry__path_basename(path, ".tar.gz");
    TEST_CHECK_STRING_EQUAL(base->path, "/foo/bar");
    sentry__path_free(base);
    sentry__path_free(path);

    // non-matching suffix returns NULL
    path = sentry__path_from_str("/foo/bar.envelope");
    TEST_CHECK(!sentry__path_basename(path, ".dmp"));
    sentry__path_free(path);

    // NULL suffix strips at last dot
    path = sentry__path_from_str("/foo/bar.envelope");
    base = sentry__path_basename(path, NULL);
    TEST_CHECK_STRING_EQUAL(base->path, "/foo/bar");
    sentry__path_free(base);
    sentry__path_free(path);

    // NULL suffix on compound extension strips only last
    path = sentry__path_from_str("/foo/bar.tar.gz");
    base = sentry__path_basename(path, NULL);
    TEST_CHECK_STRING_EQUAL(base->path, "/foo/bar.tar");
    sentry__path_free(base);
    sentry__path_free(path);

    // NULL suffix with no extension returns NULL
    path = sentry__path_from_str("/foo/bar");
    TEST_CHECK(!sentry__path_basename(path, NULL));
    sentry__path_free(path);

    // NULL suffix with dotfile returns NULL
    path = sentry__path_from_str("/foo/.hidden");
    TEST_CHECK(!sentry__path_basename(path, NULL));
    sentry__path_free(path);

    // suffix longer than path returns NULL
    path = sentry__path_from_str(".gz");
    TEST_CHECK(!sentry__path_basename(path, ".tar.gz"));
    sentry__path_free(path);
}

SENTRY_TEST(path_unique)
{
    sentry_path_t *dir
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".unique-name");
    TEST_ASSERT(!!dir);
    sentry__path_remove_all(dir);
    sentry__path_create_dir_all(dir);

    // free name returns unchanged
    sentry_path_t *path = sentry__path_unique(dir, "foo.txt");
    TEST_ASSERT(!!path);
    TEST_CHECK_STRING_EQUAL(sentry__path_filename(path), "foo.txt");
    sentry__path_free(path);

    // first collision bumps to -1 before the extension
    sentry_path_t *taken = sentry__path_join_str(dir, "foo.txt");
    sentry__path_write_buffer(taken, "x", 1);
    sentry__path_free(taken);
    path = sentry__path_unique(dir, "foo.txt");
    TEST_ASSERT(!!path);
    TEST_CHECK_STRING_EQUAL(sentry__path_filename(path), "foo-1.txt");
    sentry__path_free(path);

    // subsequent collisions skip taken suffixes in order
    taken = sentry__path_join_str(dir, "foo-1.txt");
    sentry__path_write_buffer(taken, "x", 1);
    sentry__path_free(taken);
    path = sentry__path_unique(dir, "foo.txt");
    TEST_ASSERT(!!path);
    TEST_CHECK_STRING_EQUAL(sentry__path_filename(path), "foo-2.txt");
    sentry__path_free(path);

    // filename without extension gets a trailing index
    path = sentry__path_unique(dir, "bar");
    TEST_ASSERT(!!path);
    TEST_CHECK_STRING_EQUAL(sentry__path_filename(path), "bar");
    sentry__path_free(path);
    taken = sentry__path_join_str(dir, "bar");
    sentry__path_write_buffer(taken, "x", 1);
    sentry__path_free(taken);
    path = sentry__path_unique(dir, "bar");
    TEST_ASSERT(!!path);
    TEST_CHECK_STRING_EQUAL(sentry__path_filename(path), "bar-1");
    sentry__path_free(path);

    // leading-dot filename is treated as stem-only, not as a bare extension
    path = sentry__path_unique(dir, ".hidden");
    TEST_ASSERT(!!path);
    TEST_CHECK_STRING_EQUAL(sentry__path_filename(path), ".hidden");
    sentry__path_free(path);
    taken = sentry__path_join_str(dir, ".hidden");
    sentry__path_write_buffer(taken, "x", 1);
    sentry__path_free(taken);
    path = sentry__path_unique(dir, ".hidden");
    TEST_ASSERT(!!path);
    TEST_CHECK_STRING_EQUAL(sentry__path_filename(path), ".hidden-1");
    sentry__path_free(path);

    // NULL / empty inputs are rejected
    TEST_CHECK(!sentry__path_unique(NULL, "x"));
    TEST_CHECK(!sentry__path_unique(dir, NULL));
    TEST_CHECK(!sentry__path_unique(dir, ""));

    sentry__path_remove_all(dir);
    sentry__path_free(dir);
}

SENTRY_TEST(path_copy)
{
#if defined(SENTRY_PLATFORM_NX)
    SKIP_TEST();
#endif
    sentry_path_t *src
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".copy-src");
    TEST_ASSERT(!!src);
    sentry_path_t *dst
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".copy-dst");
    TEST_ASSERT(!!dst);

    // cleanup
    sentry__path_remove_all(src);
    sentry__path_remove_all(dst);

    // copy file with content preserved
    sentry__path_write_buffer(src, "hello", 5);
    TEST_CHECK(sentry__path_copy(src, dst) == 0);
    TEST_CHECK(sentry__path_is_file(src));
    TEST_CHECK(sentry__path_is_file(dst));
    size_t len = 0;
    char *buf = sentry__path_read_to_buffer(dst, &len);
    TEST_ASSERT(!!buf);
    TEST_CHECK(len == 5);
    TEST_CHECK(memcmp(buf, "hello", 5) == 0);
    sentry_free(buf);
    sentry__path_remove(dst);

    // overwrite existing dst
    sentry__path_write_buffer(dst, "dst-data", 8);
    TEST_CHECK(sentry__path_copy(src, dst) == 0);
    TEST_CHECK(sentry__path_is_file(src));
    buf = sentry__path_read_to_buffer(dst, &len);
    TEST_ASSERT(!!buf);
    TEST_CHECK(len == 5);
    TEST_CHECK(memcmp(buf, "hello", 5) == 0);
    sentry_free(buf);
    sentry__path_remove(dst);

    // copy nonexistent src
    sentry__path_remove(src);
    TEST_CHECK(sentry__path_copy(src, dst) != 0);

    sentry__path_remove_all(dst);
    sentry__path_free(dst);
    sentry__path_free(src);
}
