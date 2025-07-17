#include "sentry_boot.h"

// TEST_NO_MAIN may be defined by downstream SDKs that want to include unittests
// in their own test executable.
#ifndef TEST_NO_MAIN
#    define SENTRY_TEST_DEFINE_MAIN
#endif

#include "sentry_testsupport.h"

#define XX(Name) SENTRY_TEST(Name);
#include "tests.inc"
#undef XX

TEST_LIST = {
#define DECLARE_TEST(Name, Func) { Name, Func },
#define XX(Name) DECLARE_TEST(#Name, CONCAT(test_sentry_, Name))
#include "tests.inc"
#undef XX
#undef DECLARE_TEST
    { 0 }
};
