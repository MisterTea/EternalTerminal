#include "sentry_json.h"
#include "sentry_path.h"
#include "sentry_testsupport.h"
#include "sentry_value.h"

#include <string.h>

static void
parse_json_roundtrip(const sentry_path_t *path)
{
    size_t buf_len = 0;
    char *buf = sentry__path_read_to_buffer(path, &buf_len);
    if (!buf) {
        return;
    }

    // parse the incoming json
    sentry_value_t value = sentry__value_from_json(buf, buf_len);
    sentry_free(buf);

    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_sb(NULL);
    sentry__jsonwriter_write_value(jw, value);
    size_t serialized1_len = 0;
    char *serialized1 = sentry__jsonwriter_into_string(jw, &serialized1_len);
    sentry_value_decref(value);

    value = sentry__value_from_json(serialized1, serialized1_len);

    jw = sentry__jsonwriter_new_sb(NULL);
    sentry__jsonwriter_write_value(jw, value);
    size_t serialized2_len = 0;
    char *serialized2 = sentry__jsonwriter_into_string(jw, &serialized2_len);
    sentry_value_decref(value);

    TEST_CHECK_STRING_EQUAL(serialized1, serialized2);

    sentry_free(serialized1);
    sentry_free(serialized2);
}

SENTRY_TEST(fuzz_json)
{
    // skipping on platforms that don't have access to fixtures on the local FS
#if defined(SENTRY_PLATFORM_ANDROID) || defined(SENTRY_PLATFORM_NX)            \
    || defined(SENTRY_PLATFORM_PS) || defined(SENTRY_PLATFORM_XBOX)
    SKIP_TEST();
#else
    sentry_path_t *path = sentry__path_from_str(__FILE__);
    TEST_ASSERT(!!path);
    sentry_path_t *dir = sentry__path_dir(path);
    TEST_ASSERT(!!dir);
    sentry__path_free(path);
    path = sentry__path_join_str(dir, "../fuzzing-failures/");
    TEST_ASSERT(!!path);
    sentry__path_free(dir);

    size_t items = 0;
    const sentry_path_t *p;
    sentry_pathiter_t *piter = sentry__path_iter_directory(path);
    TEST_ASSERT(!!piter);
    while ((p = sentry__pathiter_next(piter)) != NULL) {
        parse_json_roundtrip(p);
        items += 1;
    }
    TEST_CHECK(items > 0);

    sentry__pathiter_free(piter);
    sentry__path_free(path);
#endif
}
