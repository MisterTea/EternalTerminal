#include "sentry_boot.h"
#include "sentry_core.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifndef SENTRY_TEST_DEFINE_MAIN
#    define TEST_NO_MAIN
#endif
#include "../vendor/acutest.h"

#define CONCAT(A, B) A##B
#define SENTRY_TEST(Name) void CONCAT(test_sentry_, Name)(void **UNUSED(state))
#define SKIP_TEST() (void)0

#define TEST_CHECK_STRING_EQUAL(Val, ReferenceVal)                             \
    do {                                                                       \
        TEST_CHECK(strcmp(Val, ReferenceVal) == 0);                            \
        TEST_MSG("Expected: %s", ReferenceVal);                                \
        TEST_MSG("Received: %s", Val);                                         \
    } while (0)

#define TEST_CHECK_JSON_VALUE(Val, ReferenceJson)                              \
    do {                                                                       \
        char *json = sentry_value_to_json(Val);                                \
        TEST_CHECK_STRING_EQUAL(json, ReferenceJson);                          \
        sentry_free(json);                                                     \
    } while (0)

#define TEST_CHECK_INT_EQUAL(A, B)                                             \
    do {                                                                       \
        long long _a = (long long)(A);                                         \
        long long _b = (long long)(B);                                         \
        TEST_CHECK_(_a == _b, "%lld == %lld", _a, _b);                         \
    } while (0)

#if __GNUC__ >= 4
// NOTE: on linux, certain functions need to be made explicitly visible
// in order for `dladdr` to correctly find their name and offset
// also, force them not to be inlined
#    define TEST_VISIBLE                                                       \
        __attribute__((visibility("default"))) __attribute__((noinline))
#elif _WIN32
// NOTE: On Windows, pointers to non-static functions seem to resolve
// to an indirection table. This causes a mismatch in tests. With static
// functions, this does not happen.
#    define TEST_VISIBLE static
#else
#    define TEST_VISIBLE
#endif
