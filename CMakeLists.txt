cmake_minimum_required (VERSION 2.8.11)
project (EternalTCP)

option(BUILD_TEST "Build all unit test" OFF) # {boolean} Turn on or off unit tests (defaults to off/false).

IF(BUILD_TEST)
  add_subdirectory(${CMAKE_SOURCE_DIR}/cmake/external/googletest)
ENDIF(BUILD_TEST)

# Enable C++-11
if(UNIX)
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -std=c++11")
endif()

# Include external directories.
#add_subdirectory(${CMAKE_SOURCE_DIR}/cmake)

# Add cmake script directory.
LIST(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Find packages
find_package(Boost COMPONENTS system REQUIRED)
find_package(Threads REQUIRED)
find_package(Glog REQUIRED)
find_package(gflags REQUIRED)
find_package(LibGCrypt REQUIRED)

# Apple specific packages
IF (APPLE)
   FIND_LIBRARY(COREFOUNDATION_LIBRARY CoreFoundation)
ELSE (APPLE)
   SET(COREFOUNDATION_LIBRARY "")
ENDIF (APPLE)

include_directories(src)

set(${PROJECT_NAME}_SRC
src/BackedReader.cpp
src/BackedWriter.cpp
src/ClientConnection.cpp
src/Connection.cpp
src/FakeSocketHandler.cpp
src/FlakyFakeSocketHandler.cpp
src/Headers.cpp
src/ProcessHelper.cpp
src/ServerClientConnection.cpp
src/ServerConnection.cpp
src/SocketHandler.cpp
src/UnixSocketHandler.cpp
)

# Build the static library.
add_library(${PROJECT_NAME}-static STATIC ${${PROJECT_NAME}_SRC})
# Build the shared library.
#add_library(${PROJECT_NAME} SHARED ${${PROJECT_NAME}_SRC})

target_link_libraries(
  ${PROJECT_NAME}-static
  )

# Make sure the compiler can find include files for our Hello library
# when other libraries or executables link to Hello
target_include_directories (${PROJECT_NAME}-static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS ${PROJECT_NAME}-static DESTINATION lib)
install(FILES EternalTCP.hpp DESTINATION include)

enable_testing() # TODO: Temporary until gunit works
add_subdirectory(sandbox)

if(BUILD_TEST) # Build unit tests.
  message(STATUS "Enabling test for ${PROJECT_NAME}")
  enable_testing()
  add_subdirectory(test)
endif()
