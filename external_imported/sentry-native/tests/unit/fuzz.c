/*
Following https://github.com/google/AFL/blob/master/docs/QuickStartGuide.txt

Compile this via (optionally using AFL_USE_ASAN=1, but that is horribly slow):

CC=afl-clang CXX=afl-clang++ cmake -B fuzzing \
    -D CMAKE_RUNTIME_OUTPUT_DIRECTORY=$(pwd)/fuzzing \
    -D SENTRY_BACKEND=none \
    -D CMAKE_BUILD_TYPE=Release

cmake --build fuzzing --parallel --target sentry_fuzz_json

And then run:

afl-fuzz -i fuzzing-examples -o fuzzing-results -- fuzzing/sentry_fuzz_json @@

*/

#undef NDEBUG

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    define NOMINMAX
#    define _CRT_SECURE_NO_WARNINGS
#endif

#include <assert.h>
#include <string.h>

#include "sentry_json.h"
#include "sentry_path.h"
#include "sentry_value.h"

int
main(int argc, char **argv)
{
    if (argc != 2) {
        return 1;
    }
    const char *filename = argv[1];

    sentry_path_t *path = sentry__path_from_str(filename);

    size_t buf_len = 0;
    char *buf = sentry__path_read_to_buffer(path, &buf_len);
    sentry__path_free(path);
    if (!buf) {
        return 0;
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

    assert(serialized1_len == serialized2_len
        && memcmp(serialized1, serialized2, serialized1_len) == 0);

    sentry_free(serialized1);
    sentry_free(serialized2);
    return 0;
}
