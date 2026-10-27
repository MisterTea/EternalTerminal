#include "sentry_slice.h"
#include "sentry_testsupport.h"

SENTRY_TEST(slice)
{
    char buf[] = "my string buffer";
    char sbuf[] = "string";

    // we don’t have explicit slicing functions, so create the slices manually
    sentry_slice_t my = { buf, 2 };
    sentry_slice_t str1 = { buf + 3, 6 };
    sentry_slice_t str2 = { sbuf, 6 };

    TEST_CHECK(sentry__slice_eq(str1, str2));
    TEST_CHECK(!sentry__slice_eq(str1, my));

    char *owned = sentry__slice_to_owned(str1);
    TEST_ASSERT(!!owned);
    TEST_CHECK_STRING_EQUAL(owned, "string");
    sentry_free(owned);
}

SENTRY_TEST(slice_consume_uint64)
{
    uint64_t value = 0;

    sentry_slice_t zero = sentry__slice_from_str("0:foo-bar");
    value = 0;
    TEST_CHECK(sentry__slice_consume_uint64(&zero, &value));
    TEST_CHECK_UINT64_EQUAL(value, 0);
    TEST_CHECK_UINT64_EQUAL(zero.len, 8);
    TEST_CHECK_STRING_EQUAL(zero.ptr, ":foo-bar");

    sentry_slice_t max = sentry__slice_from_str("18446744073709551615:foo-bar");
    value = 0;
    TEST_CHECK(sentry__slice_consume_uint64(&max, &value));
    TEST_CHECK_UINT64_EQUAL(value, UINT64_MAX);
    TEST_CHECK_UINT64_EQUAL(max.len, 8);
    TEST_CHECK_STRING_EQUAL(max.ptr, ":foo-bar");

    sentry_slice_t negative = sentry__slice_from_str("-1:foo-bar");
    value = 0;
    TEST_CHECK(!sentry__slice_consume_uint64(&negative, &value));
    TEST_CHECK_UINT64_EQUAL(value, 0);
    TEST_CHECK_UINT64_EQUAL(negative.len, 10);
    TEST_CHECK_STRING_EQUAL(negative.ptr, "-1:foo-bar");
}
