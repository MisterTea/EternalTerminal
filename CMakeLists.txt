cmake_minimum_required (VERSION 2.8.11)
project (EternalTCP)
enable_testing()

# Enable C++-11
if(UNIX)
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -std=gnu++0x")
endif()

find_package(Boost 1.36.0)
if(Boost_FOUND)
  include_directories(${Boost_INCLUDE_DIRS})
endif()

add_subdirectory (src)
add_subdirectory (test)
