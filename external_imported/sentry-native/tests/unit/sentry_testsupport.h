#ifndef SENTRY_TEST_SUPPORT_H_INCLUDED
#define SENTRY_TEST_SUPPORT_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_core.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if !defined(SENTRY_TEST_DEFINE_MAIN) && !defined(TEST_NO_MAIN)
#    define TEST_NO_MAIN
#endif
#include "../vendor/acutest.h"

#define CONCAT(A, B) A##B
#define SENTRY_TEST(Name) void CONCAT(test_sentry_, Name)(void)
#define SKIP_TEST() (void)0

#define TEST_CHECK_STRING_EQUAL(Val, ReferenceVal)                             \
    do {                                                                       \
        TEST_CHECK(strcmp(Val, ReferenceVal) == 0);                            \
        TEST_MSG("Expected: %s", ReferenceVal);                                \
        TEST_MSG("Received: %s", Val);                                         \
    } while (0)

#define TEST_CHECK_WSTRING_EQUAL(Val, ReferenceVal)                            \
    do {                                                                       \
        TEST_CHECK(wcscmp(Val, ReferenceVal) == 0);                            \
        TEST_MSG("Expected: %s", ReferenceVal);                                \
        TEST_MSG("Received: %s", Val);                                         \
    } while (0)

#define TEST_CHECK_JSON_VALUE(Val, ReferenceJson)                              \
    do {                                                                       \
        char *json = sentry_value_to_json(Val);                                \
        TEST_ASSERT(!!json);                                                   \
        TEST_CHECK_STRING_EQUAL(json, ReferenceJson);                          \
        sentry_free(json);                                                     \
    } while (0)

#define TEST_CHECK_INT_EQUAL(A, B)                                             \
    do {                                                                       \
        long long _a = (long long)(A);                                         \
        long long _b = (long long)(B);                                         \
        TEST_CHECK_(_a == _b, "%lld == %lld", _a, _b);                         \
    } while (0)

#define TEST_ASSERT_INT_EQUAL(A, B)                                            \
    do {                                                                       \
        long long _a = (long long)(A);                                         \
        long long _b = (long long)(B);                                         \
        TEST_ASSERT_(_a == _b, "%lld == %lld", _a, _b);                        \
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
#    define TEST_VISIBLE static __declspec(noinline)
#else
#    define TEST_VISIBLE
#endif

#ifdef SENTRY_TEST_PATH_PREFIX
#    define SENTRY_TEST_OPTIONS_NEW(Varname)                                   \
        sentry_options_t *Varname = sentry_options_new();                      \
        TEST_ASSERT(!!Varname);                                                \
        sentry_options_set_database_path(                                      \
            Varname, SENTRY_TEST_PATH_PREFIX ".sentry-native")
#else
#    define SENTRY_TEST_PATH_PREFIX ""
#    define SENTRY_TEST_OPTIONS_NEW(Varname)                                   \
        sentry_options_t *Varname = sentry_options_new();                      \
        TEST_ASSERT(!!Varname)
#endif

#define SENTRY_TEST_DSN_NEW_DEFAULT(Varname)                                   \
    sentry_dsn_t *Varname = sentry__dsn_new("https://foo@sentry.invalid/42");  \
    TEST_ASSERT(!!Varname)
#define SENTRY_TEST_DSN_NEW(Varname, DSN_URL)                                  \
    sentry_dsn_t *Varname = sentry__dsn_new(DSN_URL);                          \
    TEST_ASSERT(!!Varname)

#endif // SENTRY_TEST_SUPPORT_H_INCLUDED
