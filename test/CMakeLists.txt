
find_package(Threads REQUIRED)
# fixes issue about pthread missing
if(CMAKE_USE_PTHREADS_INIT)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

include_directories(${GTEST_INC_DIR})
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
  add_dependencies("${TESTPREFIX}${srcfile}" googletest_external)
  add_dependencies("${TESTPREFIX}${srcfile}" ${PROJECT_NAME}-static)

  if(CMAKE_COMPILER_IS_GNUCXX)
    target_link_libraries("${TESTPREFIX}${srcfile}"
      ${CMAKE_THREAD_LIBS_INIT}
      ${GTEST_LIB_DIR}/libgtest.a
      ${GTEST_LIB_DIR}/libgtest_main.a
      ${GMOCK_LIB_DIR}/libgmock.a
      ${GMOCK_LIB_DIR}/libgmock_main.a
      ${PROJECT_NAME}-static
      )
    add_test(NAME ${TESTPREFIX}${srcfile} COMMAND ${TESTPREFIX}${srcfile})
  endif()
  #install (TARGETS ${TESTPREFIX}${srcfile} DESTINATION ${CMAKE_BINARY_DIR})
ENDFOREACH()

