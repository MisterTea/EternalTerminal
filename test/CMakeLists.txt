
# set the path for all test files
set(GTEST_PATH ${PROJECT_SOURCE_DIR}/test)
#
# Add your unit tests here
#
set(GTEST_SRC
  test_template.cpp
 )
# Add test executable target
set(TESTPREFIX gtest_)
foreach(SRC_LINE ${GTEST_SRC})
  string(REPLACE ".cpp" "" FILE_LINE ${SRC_LINE})

  add_executable(${FILE_LINE} ${SRC_LINE})
  add_dependencies(${FILE_LINE} ${PROJECT_NAME}-static gtest gtest_main gmock gmock_main)

  target_link_libraries(
    ${FILE_LINE}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PROJECT_NAME}-static
    gtest
    gtest_main
    gmock
    gmock_main
    ${GLOG_LIBRARIES}
    ${GFLAGS_LIBRARIES}
    ${sodium_LIBRARY_RELEASE}
    util
    )
    add_test(${FILE_LINE} ${FILE_LINE})
endforeach()
