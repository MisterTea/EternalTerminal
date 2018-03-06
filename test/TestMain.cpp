#include "Headers.hpp"

#include "gtest/gtest.h"

int main(int argc, char **argv) {
  srand(1);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
