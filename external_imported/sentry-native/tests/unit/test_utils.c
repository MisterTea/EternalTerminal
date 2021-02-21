#include "sentry_os.h"
#include "sentry_testsupport.h"
#include "sentry_utils.h"
#include "sentry_value.h"
#include <sentry.h>

#ifdef SENTRY_PLATFORM_UNIX
#    include "sentry_unix_pageallocator.h"
#endif

SENTRY_TEST(iso_time)
{
    uint64_t msec;
    char *str;

    msec = sentry__iso8601_to_msec("1970-01-01T00:00:10Z");
    TEST_CHECK_INT_EQUAL(msec, 10 * 1000);
    msec = sentry__iso8601_to_msec("2020-04-27T11:02:36.050Z");
    TEST_CHECK_INT_EQUAL(msec, 1587985356050);
    str = sentry__msec_time_to_iso8601(msec);
    TEST_CHECK_STRING_EQUAL(str, "2020-04-27T11:02:36.050Z");
    sentry_free(str);

    msec = sentry__msec_time();
    str = sentry__msec_time_to_iso8601(msec);
    uint64_t roundtrip = sentry__iso8601_to_msec(str);
    sentry_free(str);
    TEST_CHECK_INT_EQUAL(roundtrip, msec);
}

SENTRY_TEST(url_parsing_complete)
{
    sentry_url_t url;
    TEST_CHECK_INT_EQUAL(
        sentry__url_parse(
            &url, "http://username:password@example.com/foo/bar?x=y#z"),
        0);
    TEST_CHECK_STRING_EQUAL(url.scheme, "http");
    TEST_CHECK_STRING_EQUAL(url.host, "example.com");
    TEST_CHECK_INT_EQUAL(url.port, 80);
    TEST_CHECK_STRING_EQUAL(url.username, "username");
    TEST_CHECK_STRING_EQUAL(url.password, "password");
    TEST_CHECK_STRING_EQUAL(url.path, "/foo/bar");
    TEST_CHECK_STRING_EQUAL(url.query, "x=y");
    TEST_CHECK_STRING_EQUAL(url.fragment, "z");
    sentry__url_cleanup(&url);
}

SENTRY_TEST(url_parsing_partial)
{
    sentry_url_t url;
    TEST_CHECK_INT_EQUAL(
        sentry__url_parse(&url, "http://username:password@example.com/foo/bar"),
        0);
    TEST_CHECK_STRING_EQUAL(url.scheme, "http");
    TEST_CHECK_STRING_EQUAL(url.host, "example.com");
    TEST_CHECK_INT_EQUAL(url.port, 80);
    TEST_CHECK_STRING_EQUAL(url.username, "username");
    TEST_CHECK_STRING_EQUAL(url.password, "password");
    TEST_CHECK_STRING_EQUAL(url.path, "/foo/bar");
    TEST_CHECK(url.query == NULL);
    TEST_CHECK(url.fragment == NULL);
    sentry__url_cleanup(&url);
}

SENTRY_TEST(url_parsing_invalid)
{
    sentry_url_t url;
    TEST_CHECK_INT_EQUAL(sentry__url_parse(&url, "http:"), 1);
}

SENTRY_TEST(dsn_parsing_complete)
{
    sentry_dsn_t *dsn = sentry__dsn_new(
        "http://username:password@example.com/foo/bar/42?x=y#z");
    TEST_CHECK(!!dsn);
    if (!dsn) {
        return;
    }
    TEST_CHECK(dsn->is_valid);
    TEST_CHECK(!dsn->is_secure);
    TEST_CHECK_STRING_EQUAL(dsn->host, "example.com");
    TEST_CHECK_INT_EQUAL(dsn->port, 80);
    TEST_CHECK_STRING_EQUAL(dsn->public_key, "username");
    TEST_CHECK_STRING_EQUAL(dsn->secret_key, "password");
    TEST_CHECK_STRING_EQUAL(dsn->path, "/foo/bar");
    TEST_CHECK_INT_EQUAL((int)dsn->project_id, 42);
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_parsing_invalid)
{
    sentry_dsn_t *dsn
        = sentry__dsn_new("http://username:password@example.com/foo/bar?x=y#z");
    TEST_CHECK(!!dsn);
    if (!dsn) {
        return;
    }
    TEST_CHECK(!dsn->is_valid);
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_store_url_with_path)
{
    sentry_dsn_t *dsn = sentry__dsn_new(
        "http://username:password@example.com/foo/bar/42?x=y#z");
    char *url;
    url = sentry__dsn_get_envelope_url(dsn);
    TEST_CHECK_STRING_EQUAL(
        url, "http://example.com:80/foo/bar/api/42/envelope/");
    sentry_free(url);
    url = sentry__dsn_get_minidump_url(dsn);
    TEST_CHECK_STRING_EQUAL(url,
        "http://example.com:80/foo/bar/api/42/minidump/"
        "?sentry_client=" SENTRY_SDK_USER_AGENT "&sentry_key=username");
    sentry_free(url);
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(dsn_store_url_without_path)
{
    sentry_dsn_t *dsn
        = sentry__dsn_new("http://username:password@example.com/42?x=y#z");
    char *url;
    url = sentry__dsn_get_envelope_url(dsn);
    TEST_CHECK_STRING_EQUAL(url, "http://example.com:80/api/42/envelope/");
    sentry_free(url);
    url = sentry__dsn_get_minidump_url(dsn);
    TEST_CHECK_STRING_EQUAL(url,
        "http://example.com:80/api/42/minidump/"
        "?sentry_client=" SENTRY_SDK_USER_AGENT "&sentry_key=username");
    sentry_free(url);
    sentry__dsn_decref(dsn);
}

SENTRY_TEST(page_allocator)
{
#ifndef SENTRY_PLATFORM_UNIX
    SKIP_TEST();
#else
    const size_t size = 4096;
    char *p_before = sentry_malloc(size);
    for (size_t i = 0; i < size; i++) {
        p_before[i] = i % 255;
    }
    sentry__page_allocator_enable();

    char *p_after = sentry_malloc(size);
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
    TEST_CHECK(sentry_value_get_type(sentry_value_get_by_key(os, "version"))
        == SENTRY_VALUE_TYPE_STRING);

    sentry_value_decref(os);
}
