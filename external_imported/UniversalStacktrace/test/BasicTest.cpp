#include "Catch2/include/catch.hpp"

#include "ust.hpp"

inline void checkBetween(int x, int low, int high) {
  REQUIRE(x >= low);
  REQUIRE(x <= high);
}

void f2();
void f();

void f() { f2(); }

void f2() {
  auto traceEntries = ust::generate();
  std::cout << traceEntries << std::endl;
  std::string fileName = std::string(__FILE__);
  fileName = ust::ustBasenameString(fileName);
  REQUIRE(ust::ustBasenameString(traceEntries.entries[0].sourceFileName) ==
          fileName);
  checkBetween(traceEntries.entries[0].lineNumber, __LINE__ - 6, __LINE__ - 5);
  REQUIRE(ust::ustBasenameString(traceEntries.entries[1].sourceFileName) ==
          fileName);
  REQUIRE(traceEntries.entries[1].lineNumber == __LINE__ - 12);
  REQUIRE(ust::ustBasenameString(traceEntries.entries[2].sourceFileName) ==
          fileName);
  REQUIRE(traceEntries.entries[2].lineNumber == __LINE__ + 3);
}

TEST_CASE("ConnectionTest", "[ConnectionTest]") { f(); }
