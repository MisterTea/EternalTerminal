#define SENTRY_TEST_DEFINE_MAIN

#include "sentry_testsupport.h"

#define XX(Name) void CONCAT(test_sentry_, Name)(void);
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
