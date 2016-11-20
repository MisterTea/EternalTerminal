#include "gtest/gtest.h"

TEST ( A, B ) { SUCCEED ( ); }
int main ( int argc, char **argv ) {
  testing::InitGoogleTest ( &argc, argv );
  return RUN_ALL_TESTS ( );
}
