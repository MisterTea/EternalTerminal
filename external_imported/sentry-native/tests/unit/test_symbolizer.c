#include "sentry_symbolizer.h"
#include "sentry_testsupport.h"

// On arm64e, function pointers have PAC (Pointer Authentication Code) bits
// that must be stripped for comparison with addresses from dladdr().
#if defined(__arm64e__)
#    include <ptrauth.h>
#    define STRIP_PAC_FROM_FPTR(fptr)                                          \
        ptrauth_strip(fptr, ptrauth_key_function_pointer)
#else
#    define STRIP_PAC_FROM_FPTR(fptr) (fptr)
#endif

TEST_VISIBLE void
test_function(void)
{
    printf("Something here\n");
}

static void
asserter(const sentry_frame_info_t *info, void *data)
{
    int *called = data;
    TEST_CHECK(!!info->symbol);
    TEST_CHECK(!!info->object_name);
    TEST_CHECK(info->symbol && strstr(info->symbol, "test_function") != 0);
    TEST_CHECK(info->object_name
        && strstr(info->object_name, "sentry_test_unit") != 0);
#ifdef SENTRY_PLATFORM_AIX
    // Again, function descriptors. Should be enabled for ELFv1 PPC too.
    TEST_CHECK(info->symbol_addr == *(void **)&test_function);
    TEST_CHECK(
        info->instruction_addr == ((char *)*(void **)&test_function) + 1);
#else
    void *expected_addr = STRIP_PAC_FROM_FPTR((void *)&test_function);
    TEST_CHECK(info->symbol_addr == expected_addr);
    TEST_CHECK(info->instruction_addr == ((char *)expected_addr) + 1);
#endif
    *called += 1;
}

SENTRY_TEST(symbolizer)
{
#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_XBOX)
    SKIP_TEST();
#endif

    int called = 0;
#ifdef SENTRY_PLATFORM_AIX
    sentry__symbolize(
        ((char *)*(void **)&test_function) + 1, asserter, &called);
#else
    void *func_addr = STRIP_PAC_FROM_FPTR((void *)&test_function);
    sentry__symbolize(((char *)func_addr) + 1, asserter, &called);
#endif
    TEST_CHECK_INT_EQUAL(called, 1);
}
