set(GTEST_PATH ${PROJECT_SOURCE_DIR}/test)

file(GLOB TEST_SRCS *Test.cpp )
add_executable(
  et-test

  ${TEST_SRCS}
  Main.cpp
  )

add_dependencies(
  et-test
  TerminalCommon
  et-lib
  gtest
  gtest_main
  gmock
  gmock_main)

target_link_libraries(
  et-test
  TerminalCommon
  et-lib
  ${CMAKE_THREAD_LIBS_INIT}
  gtest
  gtest_main
  gmock
  gmock_main
  ${GFLAGS_LIBRARIES}
  ${sodium_LIBRARY_RELEASE}
  resolv
  util
  )
add_test(
  et-test
  et-test
  )
