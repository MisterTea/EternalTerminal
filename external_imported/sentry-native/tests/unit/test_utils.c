#include "sentry_os.h"
#include "sentry_slice.h"
#include "sentry_string.h"
#include "sentry_testsupport.h"
#include "sentry_utils.h"
#include "sentry_value.h"
#include <stdlib.h>
#include <string.h>

#ifdef SENTRY_PLATFORM_UNIX
#    include "sentry_unix_pageallocator.h"
#endif

#ifdef SENTRY_PLATFORM_WINDOWS
#    define setenv(k, v, o) _putenv_s(k, v)
#    define unsetenv(k) _putenv_s(k, "")
#endif

SENTRY_TEST(iso_time)
{
    uint64_t usec;
    char *str;

    usec = sentry__iso8601_to_usec("1970-01-01T00:00:10Z");
    TEST_CHECK_INT_EQUAL(usec, 10 * 1000000);
    usec = sentry__iso8601_to_usec("2020-04-27T11:02:36.050505Z");
    TEST_CHECK_INT_EQUAL(usec, 1587985356050505);
    str = sentry__usec_time_to_iso8601(usec);
    TEST_ASSERT(!!str);
    TEST_CHECK_STRING_EQUAL(str, "2020-04-27T11:02:36.050505Z");
    sentry_free(str);

    usec = sentry__usec_time();
    str = sentry__usec_time_to_iso8601(usec);
    TEST_ASSERT(!!str);
    uint64_t roundtrip = sentry__iso8601_to_usec(str);
    sentry_free(str);
    TEST_CHECK_INT_EQUAL(roundtrip, usec);
}

static void
check_url(const sentry_url_t *url)
{
    TEST_CHECK_STRING_EQUAL(url->scheme, "http");
    TEST_CHECK_STRING_EQUAL(url->host, "example.com");
    TEST_CHECK_INT_EQUAL(url->port, 80);
    TEST_CHECK_STRING_EQUAL(url->username, "username");
    TEST_CHECK_STRING_EQUAL(url->password, "password");
}

SENTRY_TEST(url_parsing_complete)
{
    sentry_url_t url;
    TEST_ASSERT_INT_EQUAL(
        sentry__url_parse(
            &url, "http://username:password@example.com/foo/bar?x=y#z", true),
        0);
    check_url(&url);
    TEST_CHECK_STRING_EQUAL(url.path, "/foo/bar");
    TEST_CHECK_STRING_EQUAL(url.query, "x=y");
    TEST_CHECK_STRING_EQUAL(url.fragment, "z");
    sentry__url_cleanup(&url);
}

SENTRY_TEST(url_parsing_partial)
{
    sentry_url_t url;
    TEST_ASSERT_INT_EQUAL(
        sentry__url_parse(
            &url, "http://username:password@example.com/foo/bar", true),
        0);
    check_url(&url);
    TEST_CHECK_STRING_EQUAL(url.path, "/foo/bar");
    TEST_CHECK(url.query == NULL);
    TEST_CHECK(url.fragment == NULL);
    sentry__url_cleanup(&url);
}

SENTRY_TEST(url_parsing_invalid)
{
    sentry_url_t url;
    TEST_ASSERT_INT_EQUAL(sentry__url_parse(&url, "http:", true), 1);
}

SENTRY_TEST(url_parsing_no_path)
{
    sentry_url_t url;
    TEST_ASSERT_INT_EQUAL(
        sentry__url_parse(&url, "http://username:password@example.com", false),
        0);
    check_url(&url);
    sentry__url_cleanup(&url);
}

SENTRY_TEST(url_parsing_with_path)
{
    sentry_url_t url;
    TEST_ASSERT_INT_EQUAL(
        sentry__url_parse(&url, "http://username:password@example.com", true),
        1);
}

SENTRY_TEST(dsn_parsing_complete)
{
    SENTRY_TEST_DSN_NEW(dsn,
        "http://username:password@o123456.example.com/foo/bar/42%21?x=y#z");
    TEST_CHECK(dsn->is_valid);
    TEST_CHECK(!dsn->is_secure);
    TEST_CHECK_STRING_EQUAL(dsn->host, "o123456.example.com");
    TEST_CHECK_INT_EQUAL(dsn->port, 80);
    TEST_CHECK_STRING_EQUAL(dsn->public_key, "username");
    TEST_CHECK_STRING_EQUAL(dsn->secret_key, "password");
    TEST_CHECK_STRING_EQUAL(dsn->org_id, "123456");
    TEST_CHECK_STRING_EQUAL(dsn->path, "/foo/bar");
    TEST_CHECK_STRING_EQUAL(dsn->project_id, "42%21");
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_parsing_max_org_length)
{
    // maximal org_id length
    SENTRY_TEST_DSN_NEW(dsn,
        "http://username:password@o18446744073709551615.example.com/42%21");
    TEST_CHECK(dsn->is_valid);
    TEST_CHECK(!dsn->is_secure);
    TEST_CHECK_STRING_EQUAL(dsn->host, "o18446744073709551615.example.com");
    TEST_CHECK_INT_EQUAL(dsn->port, 80);
    TEST_CHECK_STRING_EQUAL(dsn->public_key, "username");
    TEST_CHECK_STRING_EQUAL(dsn->secret_key, "password");
    TEST_CHECK_STRING_EQUAL(dsn->org_id, "18446744073709551615");
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_parsing_too_long_org_length)
{
    // longer than maximal org_id length
    SENTRY_TEST_DSN_NEW(dsn,
        "http://username:password@o18446744073709551615123.example.com/42%21");
    TEST_CHECK(dsn->is_valid);
    TEST_CHECK(!dsn->is_secure);
    TEST_CHECK_STRING_EQUAL(dsn->host, "o18446744073709551615123.example.com");
    TEST_CHECK_INT_EQUAL(dsn->port, 80);
    TEST_CHECK_STRING_EQUAL(dsn->public_key, "username");
    TEST_CHECK_STRING_EQUAL(dsn->secret_key, "password");
    // invalid org_id gets parsed as empty
    TEST_CHECK_STRING_EQUAL(dsn->org_id, "");
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_parsing_zero_id_org)
{
    // non-numerical org_id
    SENTRY_TEST_DSN_NEW(dsn, "http://username:password@o0.example.com/42%21");
    TEST_CHECK(dsn->is_valid);
    TEST_CHECK(!dsn->is_secure);
    TEST_CHECK_STRING_EQUAL(dsn->host, "o0.example.com");
    TEST_CHECK_INT_EQUAL(dsn->port, 80);
    TEST_CHECK_STRING_EQUAL(dsn->public_key, "username");
    TEST_CHECK_STRING_EQUAL(dsn->secret_key, "password");
    TEST_CHECK_STRING_EQUAL(dsn->org_id, "0");
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_parsing_incorrect_org)
{
    // non-numerical org_id
    SENTRY_TEST_DSN_NEW(
        dsn, "http://username:password@orange.example.com/42%21");
    TEST_CHECK(dsn->is_valid);
    TEST_CHECK(!dsn->is_secure);
    TEST_CHECK_STRING_EQUAL(dsn->host, "orange.example.com");
    TEST_CHECK_INT_EQUAL(dsn->port, 80);
    TEST_CHECK_STRING_EQUAL(dsn->public_key, "username");
    TEST_CHECK_STRING_EQUAL(dsn->secret_key, "password");
    // invalid org_id gets parsed as empty
    TEST_CHECK_STRING_EQUAL(dsn->org_id, "");
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_parsing_project_id_without_path)
{
    SENTRY_TEST_DSN_NEW(dsn, "https://username@example.com/42%21");
    TEST_CHECK(dsn->is_valid);
    TEST_CHECK(dsn->is_secure);
    TEST_CHECK_STRING_EQUAL(dsn->host, "example.com");
    TEST_CHECK_INT_EQUAL(dsn->port, 443);
    TEST_CHECK_STRING_EQUAL(dsn->public_key, "username");
    TEST_CHECK_STRING_EQUAL(dsn->org_id, "");
    TEST_CHECK(!dsn->secret_key);
    TEST_CHECK_STRING_EQUAL(dsn->path, "");
    TEST_CHECK_STRING_EQUAL(dsn->project_id, "42%21");
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_parsing_project_id_with_path_prefix)
{
    SENTRY_TEST_DSN_NEW(
        dsn, "https://username@example.com/pathone/pathtwo/42%21");
    TEST_CHECK(dsn->is_valid);
    TEST_CHECK(dsn->is_secure);
    TEST_CHECK_STRING_EQUAL(dsn->host, "example.com");
    TEST_CHECK_INT_EQUAL(dsn->port, 443);
    TEST_CHECK_STRING_EQUAL(dsn->public_key, "username");
    TEST_CHECK_STRING_EQUAL(dsn->org_id, "");
    TEST_CHECK(!dsn->secret_key);
    TEST_CHECK_STRING_EQUAL(dsn->path, "/pathone/pathtwo");
    TEST_CHECK_STRING_EQUAL(dsn->project_id, "42%21");
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_parsing_invalid)
{
    {
        SENTRY_TEST_DSN_NEW(dsn, "=https://foo@bar.ingest.sentry.io/1234567");
        TEST_CHECK(!dsn->is_valid);
        sentry__dsn_decref(dsn);
    }

    {
        SENTRY_TEST_DSN_NEW(dsn, "https://key@");
        TEST_CHECK(!dsn->is_valid);
        sentry__dsn_decref(dsn);
    }

    {
        SENTRY_TEST_DSN_NEW(dsn, "https://key@sentry.io");
        TEST_CHECK(!dsn->is_valid);
        sentry__dsn_decref(dsn);
    }

    {
        SENTRY_TEST_DSN_NEW(dsn, "https://sentry.io/1234567");
        TEST_CHECK(!dsn->is_valid);
        sentry__dsn_decref(dsn);
    }
}

SENTRY_TEST(dsn_store_url_with_path)
{
    SENTRY_TEST_DSN_NEW(
        dsn, "http://username:password@example.com/foo/bar/42?x=y#z");
    char *url;
    url = sentry__dsn_get_envelope_url(dsn);
    TEST_CHECK_STRING_EQUAL(
        url, "http://example.com:80/foo/bar/api/42/envelope/");
    sentry_free(url);
    url = sentry__dsn_get_minidump_url(dsn, SENTRY_SDK_USER_AGENT);
    TEST_CHECK_STRING_EQUAL(url,
        "http://example.com:80/foo/bar/api/42/minidump/"
        "?sentry_client=" SENTRY_SDK_USER_AGENT "&sentry_key=username");
    sentry_free(url);
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_store_url_without_path)
{
    SENTRY_TEST_DSN_NEW(dsn, "http://username:password@example.com/42?x=y#z");
    char *url;
    url = sentry__dsn_get_envelope_url(dsn);
    TEST_CHECK_STRING_EQUAL(url, "http://example.com:80/api/42/envelope/");
    sentry_free(url);
    url = sentry__dsn_get_minidump_url(dsn, SENTRY_SDK_USER_AGENT);
    TEST_CHECK_STRING_EQUAL(url,
        "http://example.com:80/api/42/minidump/"
        "?sentry_client=" SENTRY_SDK_USER_AGENT "&sentry_key=username");
    sentry_free(url);
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_store_url_custom_agent)
{
    SENTRY_TEST_DSN_NEW(dsn, "http://username:password@example.com/42?x=y#z");
    char *url = sentry__dsn_get_minidump_url(dsn, "custom_user_agent");
    TEST_CHECK_STRING_EQUAL(url,
        "http://example.com:80/api/42/minidump/"
        "?sentry_client=custom_user_agent&sentry_key=username");
    sentry_free(url);
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_resolve_url)
{
    SENTRY_TEST_DSN_NEW(dsn, "https://key@sentry.io/42");
    char *url;

    // relative path gets origin prepended
    url = sentry__dsn_resolve_url(dsn, "/api/42/upload/abc123/");
    TEST_CHECK_STRING_EQUAL(url, "https://sentry.io:443/api/42/upload/abc123/");
    sentry_free(url);

    // absolute URL passes through
    url = sentry__dsn_resolve_url(dsn, "https://other.host/path");
    TEST_CHECK_STRING_EQUAL(url, "https://other.host/path");
    sentry_free(url);

    // NULL inputs
    TEST_CHECK(!sentry__dsn_resolve_url(NULL, "/path"));
    TEST_CHECK(!sentry__dsn_resolve_url(dsn, NULL));

    sentry__dsn_decref(dsn);
}

SENTRY_TEST(page_allocator)
{
#if !defined(SENTRY_PLATFORM_UNIX) || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#else
    const size_t size = 4096;
    char *p_before = sentry_malloc(size);
    TEST_ASSERT(!!p_before);
    for (size_t i = 0; i < size; i++) {
        p_before[i] = i % 255;
    }
    sentry__page_allocator_enable();

    char *p_after = sentry_malloc(size);
    TEST_ASSERT(!!p_after);
    for (size_t i = 0; i < size; i++) {
        p_after[i] = (i + 10) % 255;
    }

    /* free is a noop after page allocator was enabled */
    sentry_free(p_before);
    sentry_free(p_after);

    for (size_t i = 0; i < size; i++) {
        TEST_CHECK_INT_EQUAL((unsigned char)p_before[i], i % 255);
        TEST_CHECK_INT_EQUAL((unsigned char)p_after[i], (i + 10) % 255);
    }

    sentry__page_allocator_disable();

    /* now we can free p_before though */
    sentry_free(p_before);
#endif
}

SENTRY_TEST(os)
{
    sentry_value_t os = sentry__get_os_context();

    TEST_CHECK(!sentry_value_is_null(os));
    TEST_CHECK(sentry_value_get_type(sentry_value_get_by_key(os, "name"))
        == SENTRY_VALUE_TYPE_STRING);
#if !defined(SENTRY_PLATFORM_NX) && !defined(SENTRY_PLATFORM_PS)
    TEST_CHECK(sentry_value_get_type(sentry_value_get_by_key(os, "version"))
        == SENTRY_VALUE_TYPE_STRING);
#endif

    sentry_value_decref(os);
}

SENTRY_TEST(check_version)
{
    TEST_CHECK(sentry__check_min_version(
        (sentry_version_t) { .major = 7, .minor = 10, .patch = 7 },
        (sentry_version_t) { .major = 7, .minor = 10, .patch = 7 }));
    TEST_CHECK(sentry__check_min_version(
        (sentry_version_t) { .major = 7, .minor = 11, .patch = 7 },
        (sentry_version_t) { .major = 7, .minor = 10, .patch = 7 }));
    TEST_CHECK(sentry__check_min_version(
        (sentry_version_t) { .major = 7, .minor = 10, .patch = 8 },
        (sentry_version_t) { .major = 7, .minor = 10, .patch = 7 }));
    TEST_CHECK(sentry__check_min_version(
        (sentry_version_t) { .major = 8, .minor = 9, .patch = 7 },
        (sentry_version_t) { .major = 7, .minor = 10, .patch = 7 }));
    TEST_CHECK(sentry__check_min_version(
        (sentry_version_t) { .major = 7, .minor = 11, .patch = 6 },
        (sentry_version_t) { .major = 7, .minor = 10, .patch = 7 }));

    TEST_CHECK(!sentry__check_min_version(
        (sentry_version_t) { .major = 6, .minor = 10, .patch = 7 },
        (sentry_version_t) { .major = 7, .minor = 10, .patch = 7 }));
    TEST_CHECK(!sentry__check_min_version(
        (sentry_version_t) { .major = 7, .minor = 9, .patch = 7 },
        (sentry_version_t) { .major = 7, .minor = 10, .patch = 7 }));
    TEST_CHECK(!sentry__check_min_version(
        (sentry_version_t) { .major = 7, .minor = 10, .patch = 6 },
        (sentry_version_t) { .major = 7, .minor = 10, .patch = 7 }));
}

SENTRY_TEST(dsn_without_url_scheme_is_invalid)
{
    SENTRY_TEST_DSN_NEW(dsn, "//without-scheme-separator");
    TEST_CHECK(dsn->is_valid == false);
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_with_non_http_scheme_is_invalid)
{
    SENTRY_TEST_DSN_NEW(dsn, "ftp://ftp-server/");
    TEST_CHECK(dsn->is_valid == false);
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_without_project_id_is_invalid)
{
    SENTRY_TEST_DSN_NEW(dsn, "https://foo@sentry.io/");
    TEST_CHECK(dsn->is_valid == false);
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_with_ending_forward_slash_will_be_cleaned)
{
    SENTRY_TEST_DSN_NEW(dsn, "https://foo@sentry.io/42/43/44////");

    TEST_CHECK_STRING_EQUAL(dsn->path, "/42/43");
    TEST_CHECK_STRING_EQUAL(dsn->project_id, "44");
    TEST_CHECK(dsn->is_valid == true);

    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_auth_header_no_user_agent)
{
    SENTRY_TEST_DSN_NEW(dsn, "https://key@sentry.io/42");
    char *auth_header = sentry__dsn_get_auth_header(dsn, NULL);
    TEST_ASSERT(!!auth_header);
    TEST_CHECK_STRING_EQUAL(auth_header,
        "Sentry sentry_key=key, sentry_version=7, "
        "sentry_client=" SENTRY_SDK_NAME "/" SENTRY_SDK_VERSION);

    sentry_free(auth_header);
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_auth_header_custom_user_agent)
{
    SENTRY_TEST_DSN_NEW(dsn, "https://key@sentry.io/42");
    char *auth_header = sentry__dsn_get_auth_header(dsn, "user_agent");
    TEST_ASSERT(!!auth_header);
    TEST_CHECK_STRING_EQUAL(auth_header,
        "Sentry sentry_key=key, sentry_version=7, "
        "sentry_client=user_agent");

    sentry_free(auth_header);
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_auth_header_null_dsn)
{
    char *auth_header = sentry__dsn_get_auth_header(NULL, NULL);
    TEST_CHECK(!auth_header);
}

SENTRY_TEST(dsn_auth_header_invalid_dsn)
{
    sentry_dsn_t *dsn = sentry__dsn_new("whatever");
    char *auth_header = sentry__dsn_get_auth_header(dsn, NULL);
    TEST_CHECK(!auth_header);

    sentry_free(auth_header);
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(getenv_double)
{
#if defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#else
    setenv("SENTRY_TEST_DOUBLE", "", 1);
    TEST_CHECK(sentry__getenv_double("SENTRY_TEST_DOUBLE", 42.0) == 42.0);

    setenv("SENTRY_TEST_DOUBLE", "0.5", 1);
    TEST_CHECK(sentry__getenv_double("SENTRY_TEST_DOUBLE", 99.0) == 0.5);

    setenv("SENTRY_TEST_DOUBLE", "-3.14", 1);
    TEST_CHECK(sentry__getenv_double("SENTRY_TEST_DOUBLE", 99.0) == -3.14);

    setenv("SENTRY_TEST_DOUBLE", "0", 1);
    TEST_CHECK(sentry__getenv_double("SENTRY_TEST_DOUBLE", 99.0) == 0.0);

    setenv("SENTRY_TEST_DOUBLE", " 1.0 \t", 1);
    TEST_CHECK(sentry__getenv_double("SENTRY_TEST_DOUBLE", 99.0) == 1.0);

    setenv("SENTRY_TEST_DOUBLE", "not_a_number", 1);
    TEST_CHECK(sentry__getenv_double("SENTRY_TEST_DOUBLE", 42.0) == 42.0);

    setenv("SENTRY_TEST_DOUBLE", "NaN", 1);
    TEST_CHECK(sentry__getenv_double("SENTRY_TEST_DOUBLE", 42.0) == 42.0);

    setenv("SENTRY_TEST_DOUBLE", "inf", 1);
    TEST_CHECK(sentry__getenv_double("SENTRY_TEST_DOUBLE", 42.0) == 42.0);

    setenv("SENTRY_TEST_DOUBLE", "-inf", 1);
    TEST_CHECK(sentry__getenv_double("SENTRY_TEST_DOUBLE", 42.0) == 42.0);

    unsetenv("SENTRY_TEST_DOUBLE");
    TEST_CHECK(sentry__getenv_double("SENTRY_TEST_DOUBLE", 42.0) == 42.0);
#endif
}

#define CHECK_SLICE_EQ(Slice, Str)                                             \
    do {                                                                       \
        TEST_CHECK_INT_EQUAL((Slice).len, strlen(Str));                        \
        TEST_CHECK((Slice).len == strlen(Str)                                  \
            && memcmp((Slice).ptr, (Str), (Slice).len) == 0);                  \
    } while (0)

SENTRY_TEST(baggage_iter_basic)
{
    const char *hdr = "a=1,b=2,c=3";
    sentry_slice_t remaining = { hdr, strlen(hdr) };
    sentry_slice_t key, val;

    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "a");
    CHECK_SLICE_EQ(val, "1");
    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "b");
    CHECK_SLICE_EQ(val, "2");
    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "c");
    CHECK_SLICE_EQ(val, "3");
    TEST_CHECK(!sentry__baggage_iter_next(&remaining, &key, &val));
}

SENTRY_TEST(baggage_iter_ows_trimmed)
{
    // Per W3C baggage, optional whitespace around keys, values, and commas
    // must be ignored.
    const char *hdr = "  a = 1 ,\tb=2  ,  c =\t3\t";
    sentry_slice_t remaining = { hdr, strlen(hdr) };
    sentry_slice_t key, val;

    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "a");
    CHECK_SLICE_EQ(val, "1");
    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "b");
    CHECK_SLICE_EQ(val, "2");
    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "c");
    CHECK_SLICE_EQ(val, "3");
    TEST_CHECK(!sentry__baggage_iter_next(&remaining, &key, &val));
}

SENTRY_TEST(baggage_iter_empty_and_malformed_skipped)
{
    // Missing `=`, empty keys, and bare commas are all skipped; valid
    // members on either side still yield.
    const char *hdr = ",malformed,  ,=orphan,a=1,=,bare,b=2,";
    sentry_slice_t remaining = { hdr, strlen(hdr) };
    sentry_slice_t key, val;

    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "a");
    CHECK_SLICE_EQ(val, "1");
    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "b");
    CHECK_SLICE_EQ(val, "2");
    TEST_CHECK(!sentry__baggage_iter_next(&remaining, &key, &val));
}

SENTRY_TEST(baggage_iter_empty_value_allowed)
{
    // Empty values are valid per spec.
    const char *hdr = "a=,b=x";
    sentry_slice_t remaining = { hdr, strlen(hdr) };
    sentry_slice_t key, val;

    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "a");
    TEST_CHECK_INT_EQUAL(val.len, 0);
    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "b");
    CHECK_SLICE_EQ(val, "x");
    TEST_CHECK(!sentry__baggage_iter_next(&remaining, &key, &val));
}

SENTRY_TEST(baggage_iter_properties_stripped)
{
    // Value ends at the first `;`; property text is discarded.
    const char *hdr = "a=1;prop=x;q,b=2;meta,c=3";
    sentry_slice_t remaining = { hdr, strlen(hdr) };
    sentry_slice_t key, val;

    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "a");
    CHECK_SLICE_EQ(val, "1");
    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "b");
    CHECK_SLICE_EQ(val, "2");
    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "c");
    CHECK_SLICE_EQ(val, "3");
    TEST_CHECK(!sentry__baggage_iter_next(&remaining, &key, &val));
}

SENTRY_TEST(baggage_iter_equals_in_value)
{
    // Only the first `=` separates key from value; subsequent ones are
    // part of the value.
    const char *hdr = "a=x=y=z";
    sentry_slice_t remaining = { hdr, strlen(hdr) };
    sentry_slice_t key, val;

    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "a");
    CHECK_SLICE_EQ(val, "x=y=z");
    TEST_CHECK(!sentry__baggage_iter_next(&remaining, &key, &val));
}

SENTRY_TEST(baggage_iter_empty_input)
{
    sentry_slice_t remaining = { "", 0 };
    sentry_slice_t key, val;
    TEST_CHECK(!sentry__baggage_iter_next(&remaining, &key, &val));

    const char *hdr = "   ";
    remaining = (sentry_slice_t) { hdr, strlen(hdr) };
    TEST_CHECK(!sentry__baggage_iter_next(&remaining, &key, &val));

    const char *only_commas = ",,,";
    remaining = (sentry_slice_t) { only_commas, strlen(only_commas) };
    TEST_CHECK(!sentry__baggage_iter_next(&remaining, &key, &val));
}

SENTRY_TEST(baggage_iter_case_preserved)
{
    // Baggage keys are case-sensitive and the iterator must preserve case.
    const char *hdr = "Sentry-Foo=Bar,sentry-foo=baz";
    sentry_slice_t remaining = { hdr, strlen(hdr) };
    sentry_slice_t key, val;

    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "Sentry-Foo");
    CHECK_SLICE_EQ(val, "Bar");
    TEST_CHECK(sentry__baggage_iter_next(&remaining, &key, &val));
    CHECK_SLICE_EQ(key, "sentry-foo");
    CHECK_SLICE_EQ(val, "baz");
    TEST_CHECK(!sentry__baggage_iter_next(&remaining, &key, &val));
}

static char *
decode_to_owned(const char *src)
{
    size_t len = strlen(src);
    char *buf = sentry__string_clone_n(src, len);
    size_t new_len = sentry__percent_decode_inplace(buf, len);
    buf[new_len] = '\0';
    return buf;
}

SENTRY_TEST(percent_decode_basic)
{
    char *s;

    s = decode_to_owned("");
    TEST_CHECK_STRING_EQUAL(s, "");
    sentry_free(s);

    s = decode_to_owned("no-escapes_here~.");
    TEST_CHECK_STRING_EQUAL(s, "no-escapes_here~.");
    sentry_free(s);

    s = decode_to_owned("a%40b%2Cc");
    TEST_CHECK_STRING_EQUAL(s, "a@b,c");
    sentry_free(s);

    // Both lower and upper case hex digits decode the same.
    s = decode_to_owned("%2f%2F");
    TEST_CHECK_STRING_EQUAL(s, "//");
    sentry_free(s);

    // %XX decodes to one byte even when that byte is high-ASCII.
    s = decode_to_owned("%E2%98%83");
    TEST_CHECK_INT_EQUAL((unsigned char)s[0], 0xE2);
    TEST_CHECK_INT_EQUAL((unsigned char)s[1], 0x98);
    TEST_CHECK_INT_EQUAL((unsigned char)s[2], 0x83);
    TEST_CHECK_INT_EQUAL(s[3], '\0');
    sentry_free(s);
}

SENTRY_TEST(percent_decode_malformed_passed_through)
{
    char *s;

    // Non-hex digits: left as-is.
    s = decode_to_owned("%GG");
    TEST_CHECK_STRING_EQUAL(s, "%GG");
    sentry_free(s);

    s = decode_to_owned("a%Zbc");
    TEST_CHECK_STRING_EQUAL(s, "a%Zbc");
    sentry_free(s);

    // Truncated escape at end of string: left as-is.
    s = decode_to_owned("abc%");
    TEST_CHECK_STRING_EQUAL(s, "abc%");
    sentry_free(s);

    s = decode_to_owned("abc%4");
    TEST_CHECK_STRING_EQUAL(s, "abc%4");
    sentry_free(s);

    // Mid-string escape followed by non-hex: left as-is, then resumes.
    s = decode_to_owned("%4X%40");
    TEST_CHECK_STRING_EQUAL(s, "%4X@");
    sentry_free(s);
}

SENTRY_TEST(percent_decode_does_not_read_past_len)
{
    // The decoder must respect `len` even when the buffer is longer; a
    // trailing `%XX` after `len` must not be touched.
    char buf[] = "a%40b%41";
    size_t new_len = sentry__percent_decode_inplace(buf, 3);
    TEST_CHECK_INT_EQUAL(new_len, 3);
    TEST_CHECK(memcmp(buf, "a%4", 3) == 0);
    // Bytes past `len` are untouched.
    TEST_CHECK_STRING_EQUAL(buf + 3, "0b%41");
}

SENTRY_TEST(base64_encode)
{
    // Standard vectors from RFC 4648
    char *out = sentry__base64_encode("", 0);
    TEST_CHECK_STRING_EQUAL(out, "");
    sentry_free(out);

    out = sentry__base64_encode("f", 1);
    TEST_CHECK_STRING_EQUAL(out, "Zg==");
    sentry_free(out);

    out = sentry__base64_encode("fo", 2);
    TEST_CHECK_STRING_EQUAL(out, "Zm8=");
    sentry_free(out);

    out = sentry__base64_encode("foo", 3);
    TEST_CHECK_STRING_EQUAL(out, "Zm9v");
    sentry_free(out);

    out = sentry__base64_encode("foob", 4);
    TEST_CHECK_STRING_EQUAL(out, "Zm9vYg==");
    sentry_free(out);

    out = sentry__base64_encode("fooba", 5);
    TEST_CHECK_STRING_EQUAL(out, "Zm9vYmE=");
    sentry_free(out);

    out = sentry__base64_encode("foobar", 6);
    TEST_CHECK_STRING_EQUAL(out, "Zm9vYmFy");
    sentry_free(out);

    // Null output on allocation failure is handled (malloc mock not needed)
    out = sentry__base64_encode(NULL, 0);
    TEST_CHECK_STRING_EQUAL(out, "");
    sentry_free(out);
}

#undef CHECK_SLICE_EQ
