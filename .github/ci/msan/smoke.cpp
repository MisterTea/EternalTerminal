#include <cstdio>
#include <iostream>
#include <string>

#if !__has_feature(memory_sanitizer)
#error "MSan smoke test must be instrumented"
#endif

int main(int argc, char**) {
  if (argc > 1) {
    volatile int* uninitialized = new int;
    std::printf("%d\n", *uninitialized);
    delete uninitialized;
    return 0;
  }
  std::string initialized = "instrumented libc++";
  std::cout << initialized << '\n';
  return 0;
}
