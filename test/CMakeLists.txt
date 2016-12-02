
find_package(Threads REQUIRED)
# fixes issue about pthread missing
#if(CMAKE_USE_PTHREADS_INIT)
#  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
#endif()

include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR})
include_directories(${GMOCK_INC_DIR})
# set the path for all test files
set(TPATH ${PROJECT_SOURCE_DIR}/test)
#
# Add your unit tests here
#
set(GTEST_SRC
  test_template.cpp
 )
# Add test executable target
set(TESTPREFIX gtest_)
FOREACH(srcfile ${GTEST_SRC})
  add_executable(${TESTPREFIX}${srcfile} ${TPATH}/${srcfile})
  add_dependencies("${TESTPREFIX}${srcfile}" ${PROJECT_NAME}-static gtest gtest_main gmock gmock_main)

  target_link_libraries("${TESTPREFIX}${srcfile}"
    ${CMAKE_THREAD_LIBS_INIT}
    ${PROJECT_NAME}-static
    gtest
    gtest_main
    gmock
    gmock_main
    glog
    gflags
    gcrypt
    )
  add_test(NAME ${TESTPREFIX}${srcfile} COMMAND ${TESTPREFIX}${srcfile})
  #install (TARGETS ${TESTPREFIX}${srcfile} DESTINATION ${CMAKE_BINARY_DIR})
ENDFOREACH()
