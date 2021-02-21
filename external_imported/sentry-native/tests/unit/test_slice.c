#include "sentry_slice.h"
#include "sentry_testsupport.h"
#include <sentry.h>

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
    TEST_CHECK_STRING_EQUAL(owned, "string");
    sentry_free(owned);
}
