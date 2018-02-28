set(GTEST_PATH ${PROJECT_SOURCE_DIR}/test)

file(GLOB TEST_SRCS Test*.cpp )
add_executable(
  ${PROJECT_NAME}-test

  ${TEST_SRCS}
  )

add_dependencies(
  ${PROJECT_NAME}-test
  TerminalCommon
  ${PROJECT_NAME}-static
  gtest
  gtest_main
  gmock
  gmock_main)

target_link_libraries(
  ${PROJECT_NAME}-test
  TerminalCommon
  ${PROJECT_NAME}-static
  ${CMAKE_THREAD_LIBS_INIT}
  gtest
  gtest_main
  gmock
  gmock_main
  ${GLOG_LIBRARIES}
  ${GFLAGS_LIBRARIES}
  ${sodium_LIBRARY_RELEASE}
  resolv
  util
  )
add_test(
  ${PROJECT_NAME}-test
  ${PROJECT_NAME}-test
  )
