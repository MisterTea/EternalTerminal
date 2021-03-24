#define CATCH_CONFIG_RUNNER
#include "Catch2/single_include/catch2/catch.hpp"

#include <iostream>

int main(int argc, char **argv) {
  std::cout << "Beginning tests" << std::endl;
  srand(1);

  int result = Catch::Session().run(argc, argv);

  return result;
}
