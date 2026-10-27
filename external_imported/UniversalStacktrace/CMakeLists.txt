cmake_minimum_required (VERSION 3.15)
project (UniversalStackTrace VERSION 0.0.1 LANGUAGES C CXX)

LIST(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

SET(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/external/sanitizers-cmake/cmake" ${CMAKE_MODULE_PATH})
FIND_PACKAGE(Sanitizers)

# Enable C++-17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  # MSVC: enable debug info (/Zi) and keep /EHsc
  # /Zi is added via MSVC debug flags automatically; ensure PDB generation
  add_compile_options(/W3 /EHsc)
  # Ensure debug symbols even in Release for stack traces
  string(APPEND CMAKE_CXX_FLAGS_DEBUG " /Zi")
  string(APPEND CMAKE_CXX_FLAGS_RELEASE " /Zi")
  string(APPEND CMAKE_CXX_FLAGS_RELWITHDEBINFO " /Zi")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_DEBUG " /DEBUG")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " /DEBUG")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO " /DEBUG")
else()
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
  if(NOT WIN32)
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
  endif()
  # Enable debug info
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -ggdb3")
  SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -ggdb3")
endif()

IF(APPLE)
# Turn off address randomizing to get debug prints
SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-pie")
ENDIF()

include_directories(
  external
  ust
  )

IF(MINGW OR CMAKE_CXX_COMPILER MATCHES "mingw32" OR CMAKE_CXX_COMPILER MATCHES "x86_64-w64-mingw32" OR (CMAKE_CXX_COMPILER_ID MATCHES "GNU" AND CMAKE_CXX_COMPILER MATCHES "mingw"))
 SET(
   CORE_LIBRARIES

   dbghelp
 )
ELSEIF(WIN32)
SET(
  CORE_LIBRARIES

  Shlwapi
  dbghelp
)
ELSEIF(APPLE)
SET(
  CORE_LIBRARIES

  System
)
ELSE()
find_package(Libunwind)
if(LIBUNWIND_FOUND)
  SET(
    CORE_LIBRARIES

    ${LIBUNWIND_LIBRARIES}
  )
else()
  SET(
    CORE_LIBRARIES

    unwind
  )
endif()
ENDIF()

add_executable(
  ust-test

  test/Main.cpp
  test/BasicTest.cpp
)
target_link_libraries(
  ust-test

  ${CORE_LIBRARIES}
)
# Workaround for Catch2 MINSIGSTKSZ constexpr issue on glibc 2.34+ (Ubuntu 22.04)
# Define before including catch.hpp to disable POSIX signal handling
target_compile_definitions(ust-test PRIVATE CATCH_CONFIG_NO_POSIX_SIGNALS)
# Ensure proper PDB/debug settings for MSVC and prevent optimization breaking stack traces
if(MSVC)
  target_compile_options(ust-test PRIVATE /Zi)
  target_link_options(ust-test PRIVATE /DEBUG)
  # Prevent inlining and tail-call optimization that would break stacktrace line checks in tests
  target_compile_options(ust-test PRIVATE /Ob0 $<$<CONFIG:Release>:/Od> $<$<CONFIG:RelWithDebInfo>:/Od>)
else()
  # GCC/Clang: keep frame pointers and disable optimization for reliable traces
  target_compile_options(ust-test PRIVATE -O0 -fno-omit-frame-pointer -fno-inline)
endif()
enable_testing()
add_test(
  NAME ust-test
  COMMAND ust-test
)
if(NOT MSVC)
  add_sanitizers(ust-test)
endif()
