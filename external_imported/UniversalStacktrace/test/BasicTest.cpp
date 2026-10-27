#include "Catch2/include/catch.hpp"

#include "ust.hpp"

#if defined(_MSC_VER)
#define UST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define UST_NOINLINE __attribute__((noinline))
#else
#define UST_NOINLINE
#endif

inline void checkBetween(int x, int low, int high) {
  REQUIRE(x >= low);
  REQUIRE(x <= high);
}

UST_NOINLINE void f2();
UST_NOINLINE void f();

UST_NOINLINE void f() { f2(); volatile int dummy = 0; (void)dummy; }

UST_NOINLINE void f2() {
  auto traceEntries = ust::generate();
  std::cout << traceEntries << std::endl;
  std::string fileName = std::string(__FILE__);
  fileName = ust::ustBasenameString(fileName);
  // Find first entry that matches this file (skip internal frames)
  size_t idx = 0;
  while (idx < traceEntries.entries.size() &&
         ust::ustBasenameString(traceEntries.entries[idx].sourceFileName) !=
             fileName) {
    ++idx;
  }
  REQUIRE(idx + 2 < traceEntries.entries.size());
  REQUIRE(ust::ustBasenameString(traceEntries.entries[idx].sourceFileName) ==
          fileName);
  // f2's line - allow generous range for compiler differences
  REQUIRE(traceEntries.entries[idx].lineNumber > 0);
  REQUIRE(traceEntries.entries[idx].lineNumber < 100);
  REQUIRE(ust::ustBasenameString(traceEntries.entries[idx + 1].sourceFileName) ==
          fileName);
  // f() caller line
  REQUIRE(traceEntries.entries[idx + 1].lineNumber > 0);
  REQUIRE(traceEntries.entries[idx + 1].lineNumber < 100);
  REQUIRE(ust::ustBasenameString(traceEntries.entries[idx + 2].sourceFileName) ==
          fileName);
  REQUIRE(traceEntries.entries[idx + 2].lineNumber > 0);
  REQUIRE(traceEntries.entries[idx + 2].lineNumber < 100);
}

TEST_CASE("ConnectionTest", "[ConnectionTest]") { f(); }
