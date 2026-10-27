#include "sentry_testsupport.h"

#include "sentry_alloc.h"
#include "sentry_json.h"
#include "sentry_path.h"
#include "sentry_string.h"
#include "sentry_writer.h"

SENTRY_TEST(writer_string_owned_into_string)
{
    sentry_writer_t *writer = sentry__writer_new_sb(NULL);
    TEST_ASSERT(!!writer);

    TEST_CHECK(sentry__writer_write(writer, "foo", 3));
    TEST_CHECK(sentry__writer_write_char(writer, '!'));
    TEST_CHECK(!sentry__writer_has_failed(writer));
    TEST_CHECK_INT_EQUAL(sentry__writer_byte_count(writer), 4);

    size_t len = 0;
    char *str = sentry__writer_into_string(writer, &len);
    TEST_ASSERT(!!str);
    TEST_CHECK_INT_EQUAL(len, 4);
    TEST_CHECK_STRING_EQUAL(str, "foo!");
    sentry_free(str);
}

SENTRY_TEST(writer_string_external_into_string)
{
    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);

    sentry_writer_t *writer = sentry__writer_new_sb(&sb);
    TEST_ASSERT(!!writer);

    TEST_CHECK(sentry__writer_write(writer, "external", 8));

    size_t len = 0;
    char *str = sentry__writer_into_string(writer, &len);
    TEST_ASSERT(!!str);
    TEST_CHECK_INT_EQUAL(len, 8);
    TEST_CHECK_STRING_EQUAL(str, "external");

    // The caller-owned stringbuilder stays valid, but its buffer was consumed.
    TEST_CHECK_INT_EQUAL(sentry__stringbuilder_len(&sb), 0);
    TEST_CHECK(sentry__stringbuilder_append(&sb, "reuse") == 0);
    TEST_CHECK_INT_EQUAL(sentry__stringbuilder_len(&sb), 5);

    sentry_free(str);
    sentry__stringbuilder_cleanup(&sb);
}

SENTRY_TEST(writer_json_external_stringbuilder_into_string)
{
    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);

    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_sb(&sb);
    TEST_ASSERT(!!jw);

    sentry__jsonwriter_write_str(jw, "hello");

    size_t len = 0;
    char *str = sentry__jsonwriter_into_string(jw, &len);
    TEST_ASSERT(!!str);
    TEST_CHECK_INT_EQUAL(len, 7);
    TEST_CHECK_STRING_EQUAL(str, "\"hello\"");
    TEST_CHECK_INT_EQUAL(sentry__stringbuilder_len(&sb), 0);

    sentry_free(str);
    sentry__stringbuilder_cleanup(&sb);
}

SENTRY_TEST(writer_json_reset_keeps_failure)
{
    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);

    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_sb(&sb);
    TEST_ASSERT(!!jw);

    // Force the underlying byte writer to fail. `reset` must only reset JSON
    // grammar state, not the sticky output failure.
    sb.len = SIZE_MAX;
    sentry__jsonwriter_write_str(jw, "x");
    TEST_CHECK(sentry__jsonwriter_has_failed(jw));

    sb.len = 0;
    sentry__jsonwriter_reset(jw);
    TEST_CHECK(sentry__jsonwriter_has_failed(jw));

    sentry__jsonwriter_free(jw);
    sentry__stringbuilder_cleanup(&sb);
}

SENTRY_TEST(writer_json_writer_does_not_own_underlying_writer)
{
    sentry_writer_t *writer = sentry__writer_new_sb(NULL);
    TEST_ASSERT(!!writer);

    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_writer(writer);
    TEST_ASSERT(!!jw);

    sentry__jsonwriter_write_str(jw, "json");
    sentry__jsonwriter_free(jw);

    // `new_writer` is non-owning: freeing the JSON writer must not close or
    // free the underlying byte writer.
    TEST_CHECK(sentry__writer_write(writer, "!", 1));

    size_t len = 0;
    char *str = sentry__writer_into_string(writer, &len);
    TEST_ASSERT(!!str);
    TEST_CHECK_INT_EQUAL(len, 7);
    TEST_CHECK_STRING_EQUAL(str, "\"json\"!");
    sentry_free(str);
}

SENTRY_TEST(writer_close_is_idempotent)
{
    sentry_writer_t *writer = sentry__writer_new_sb(NULL);
    TEST_ASSERT(!!writer);

    TEST_CHECK(sentry__writer_write(writer, "close", 5));
    TEST_CHECK(sentry__writer_close(writer));
    TEST_CHECK(sentry__writer_close(writer));
    TEST_CHECK(!sentry__writer_has_failed(writer));

    size_t len = 0;
    char *str = sentry__writer_into_string(writer, &len);
    TEST_ASSERT(!!str);
    TEST_CHECK_INT_EQUAL(len, 5);
    TEST_CHECK_STRING_EQUAL(str, "close");
    sentry_free(str);
}

SENTRY_TEST(writer_write_after_close_fails)
{
    sentry_writer_t *writer = sentry__writer_new_sb(NULL);
    TEST_ASSERT(!!writer);

    TEST_CHECK(sentry__writer_write(writer, "x", 1));
    TEST_CHECK(sentry__writer_close(writer));
    TEST_CHECK(!sentry__writer_write(writer, "y", 1));
    TEST_CHECK(sentry__writer_has_failed(writer));

    size_t len = 123;
    char *str = sentry__writer_into_string(writer, &len);
    TEST_CHECK(!str);
    TEST_CHECK_INT_EQUAL(len, 0);
}

SENTRY_TEST(writer_string_failure_is_sticky)
{
    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);

    sentry_writer_t *writer = sentry__writer_new_sb(&sb);
    TEST_ASSERT(!!writer);

    // Force stringbuilder append to fail without needing a huge allocation.
    sb.len = SIZE_MAX;
    TEST_CHECK(!sentry__writer_write(writer, "x", 1));
    TEST_CHECK(sentry__writer_has_failed(writer));

    // Once failed, the writer remains failed and does not recover on later
    // writes even if the underlying builder state becomes usable again.
    sb.len = 0;
    TEST_CHECK(!sentry__writer_write(writer, "y", 1));
    TEST_CHECK(sentry__writer_has_failed(writer));

    size_t len = 123;
    char *str = sentry__writer_into_string(writer, &len);
    TEST_CHECK(!str);
    TEST_CHECK_INT_EQUAL(len, 0);

    sentry__stringbuilder_cleanup(&sb);
}

SENTRY_TEST(writer_byte_count_stops_after_failure)
{
    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);

    sentry_writer_t *writer = sentry__writer_new_sb(&sb);
    TEST_ASSERT(!!writer);

    TEST_CHECK(sentry__writer_write(writer, "ok", 2));
    TEST_CHECK_INT_EQUAL(sentry__writer_byte_count(writer), 2);

    sb.len = SIZE_MAX;
    TEST_CHECK(!sentry__writer_write(writer, "x", 1));
    TEST_CHECK_INT_EQUAL(sentry__writer_byte_count(writer), 2);

    sb.len = 2;
    TEST_CHECK(!sentry__writer_write(writer, "y", 1));
    TEST_CHECK_INT_EQUAL(sentry__writer_byte_count(writer), 2);

    sentry__writer_free(writer);
    sentry__stringbuilder_cleanup(&sb);
}

SENTRY_TEST(writer_file_path)
{
    const char *test_file_str = SENTRY_TEST_PATH_PREFIX "sentry_test_writer";
    sentry_path_t *path = sentry__path_from_str(test_file_str);
    TEST_ASSERT(!!path);

    sentry_writer_t *writer = sentry__writer_new_file(path);
    TEST_ASSERT(!!writer);

    TEST_CHECK(sentry__writer_write(writer, "file", 4));
    TEST_CHECK(sentry__writer_close(writer));
    TEST_CHECK(!sentry__writer_has_failed(writer));
    sentry__writer_free(writer);

    size_t len = 0;
    char *buf = sentry__path_read_to_buffer(path, &len);
    TEST_ASSERT(!!buf);
    TEST_CHECK_INT_EQUAL(len, 4);
    TEST_CHECK_STRING_EQUAL(buf, "file");

    sentry_free(buf);
    sentry__path_remove(path);
    sentry__path_free(path);
}

SENTRY_TEST(writer_filewriter_wrapper)
{
    const char *test_file_str
        = SENTRY_TEST_PATH_PREFIX "sentry_test_writer_filewriter";
    sentry_path_t *path = sentry__path_from_str(test_file_str);
    TEST_ASSERT(!!path);

    sentry_filewriter_t *fw = sentry__filewriter_new(path);
    TEST_ASSERT(!!fw);

    sentry_writer_t *writer = sentry__writer_new_filewriter(fw, false);
    TEST_ASSERT(!!writer);

    TEST_CHECK(sentry__writer_write(writer, "wrapped", 7));
    TEST_CHECK(sentry__writer_close(writer));
    TEST_CHECK(!sentry__writer_has_failed(writer));
    sentry__writer_free(writer);

    TEST_CHECK(sentry__filewriter_close(fw));
    sentry__filewriter_free(fw);

    size_t len = 0;
    char *buf = sentry__path_read_to_buffer(path, &len);
    TEST_ASSERT(!!buf);
    TEST_CHECK_INT_EQUAL(len, 7);
    TEST_CHECK_STRING_EQUAL(buf, "wrapped");

    sentry_free(buf);
    sentry__path_remove(path);
    sentry__path_free(path);
}
