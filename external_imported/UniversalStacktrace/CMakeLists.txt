cmake_minimum_required (VERSION 3.0.2)
project (UniversalStackTrace VERSION 0.0.1)

SET(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/external/sanitizers-cmake/cmake" ${CMAKE_MODULE_PATH})
FIND_PACKAGE(Sanitizers)

# Enable C++-17
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -std=c++17")

# Enable debug info
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -ggdb3")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -ggdb3")

IF(APPLE)
# Turn off address randomizing to get debug prints
SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-pie")
ENDIF()

include_directories(
  external
  ust
  )

IF(MINGW)
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
SET(
  CORE_LIBRARIES

  unwind
)
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
add_test(
  ust-test
  ust-test
)
add_sanitizers(ust-test)
