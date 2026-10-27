# Used by ET and by every target dependency, with an ordinary host triplet
# for build tools such as protoc. No system C++ runtime enters the MSan binary.
if(NOT DEFINED ENV{MSAN_LIBCXX_ROOT} OR "$ENV{MSAN_LIBCXX_ROOT}" STREQUAL "")
  message(FATAL_ERROR "MSAN_LIBCXX_ROOT must name the instrumented libc++ install")
endif()
set(CMAKE_C_COMPILER clang-18 CACHE FILEPATH "MSan C compiler")
set(CMAKE_CXX_COMPILER clang++-18 CACHE FILEPATH "MSan C++ compiler")
set(_msan_flags "-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -fPIC")
set(CMAKE_C_FLAGS_INIT "${_msan_flags}")
# Compile with explicit instrumented headers; select libc++ only at link
# time so PCH compilation has no unused -stdlib driver argument.
set(CMAKE_CXX_FLAGS_INIT "${_msan_flags} -nostdinc++ -isystem $ENV{MSAN_LIBCXX_ROOT}/include/c++/v1 -Xclang -fno-pch-timestamp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-stdlib=libc++ -fsanitize=memory -pie -L$ENV{MSAN_LIBCXX_ROOT}/lib")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-stdlib=libc++ -fsanitize=memory -L$ENV{MSAN_LIBCXX_ROOT}/lib")
