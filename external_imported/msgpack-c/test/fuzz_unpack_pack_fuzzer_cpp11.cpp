#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif //defined(__GNUC__)

#include <gtest/gtest.h>

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif //defined(__GNUC__)

#include "../fuzz/unpack_pack_fuzzer.cpp"

TEST(FUZZ_UNPACK_PACK_FUZZER, works)
{
  EXPECT_EQ(0, LLVMFuzzerTestOneInput(0, 0));
}
