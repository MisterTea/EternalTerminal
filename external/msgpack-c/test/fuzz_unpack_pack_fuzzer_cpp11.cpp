#include <gtest/gtest.h>

#include "../fuzz/unpack_pack_fuzzer.cpp"

TEST(FUZZ_UNPACK_PACK_FUZZER, works)
{
  EXPECT_EQ(0, LLVMFuzzerTestOneInput(0, 0));
}
