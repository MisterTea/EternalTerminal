enable_testing()

include_directories(${CMAKE_SOURCE_DIR})

if(${CMAKE_SYSTEM_NAME} MATCHES "HP-UX" AND ${CMAKE_CXX_COMPILER} MATCHES "aCC$")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} +e -AA")
endif()

add_executable(simple_test EXCLUDE_FROM_ALL simple_test.cc ../base64.h)
add_test(SimpleTest simple_test)

add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure DEPENDS simple_test)
