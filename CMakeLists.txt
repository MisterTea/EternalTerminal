cmake_minimum_required (VERSION 2.8.12)
project (EternalTCP)

option(BUILD_TEST "Build all unit test" OFF) # {boolean} Turn on or off unit tests (defaults to off/false).
option(BUILD_PACKAGE "Builds the package" OFF) 


set (${PROJECT_NAME}_VERSION_MAJOR 0)
set (${PROJECT_NAME}_VERSION_MINOR 0)
set (${PROJECT_NAME}_VERSION_PATCH 0)

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
find_package(Boost REQUIRED)
find_package(Threads REQUIRED)
find_package(Glog REQUIRED)
find_package(gflags REQUIRED)
find_package(LibGCrypt REQUIRED)
find_package(Protobuf REQUIRED)

# Apply gcrypt flags
add_definitions(${LIBGCRYPT_CFLAGS})
SET( CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} ${LIBGCRYPT_LIBRARIES}" )

# Apple specific packages
IF (APPLE)
   FIND_LIBRARY(COREFOUNDATION_LIBRARY CoreFoundation)
ELSE (APPLE)
   SET(COREFOUNDATION_LIBRARY "")
ENDIF (APPLE)

PROTOBUF_GENERATE_CPP(
ET_SRCS
ET_HDRS

proto/ET.proto
)

include_directories(
src
${PROTOBUF_INCLUDE_DIRS}
${CMAKE_CURRENT_BINARY_DIR}
)

set(${PROJECT_NAME}_SRC

src/BackedReader.cpp
src/BackedWriter.cpp
src/ClientConnection.cpp
src/Connection.cpp
src/CryptoHandler.cpp
src/FakeSocketHandler.cpp
src/FlakyFakeSocketHandler.cpp
src/Headers.cpp
src/ServerClientConnection.cpp
src/ServerConnection.cpp
src/SocketHandler.cpp
src/UnixSocketHandler.cpp

${ET_SRCS} ${ET_HDRS}
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

PROTOBUF_GENERATE_CPP(
ETERMINAL_SRCS
ETERMINAL_HDRS

proto/ETerminal.proto
)

add_library(
TerminalCommon
STATIC

terminal/ConsoleUtils.cpp
terminal/ProcessHelper.cpp
terminal/SocketUtils.cpp

${ETERMINAL_SRCS} ${ETERMINAL_HDRS}
)
add_dependencies(TerminalCommon EternalTCP-static)

add_executable (Terminal terminal/Terminal.cpp)
target_link_libraries (
Terminal
LINK_PUBLIC
TerminalCommon
EternalTCP-static
${CMAKE_THREAD_LIBS_INIT}
${PROTOBUF_LIBRARIES}
${GLOG_LIBRARY}
gflags
${LIBGCRYPT_LIBRARIES}
${COREFOUNDATION_LIBRARY}
util
)

add_executable (FlakyTerminal terminal/FlakyTerminal.cpp)
target_link_libraries (
FlakyTerminal
LINK_PUBLIC
TerminalCommon
EternalTCP-static
${CMAKE_THREAD_LIBS_INIT}
${PROTOBUF_LIBRARIES}
${GLOG_LIBRARY}
gflags
${LIBGCRYPT_LIBRARIES}
${COREFOUNDATION_LIBRARY}
util
)

add_executable (ETServer terminal/TerminalServer.cpp)
target_link_libraries (
ETServer
LINK_PUBLIC
TerminalCommon
EternalTCP-static
${CMAKE_THREAD_LIBS_INIT}
${PROTOBUF_LIBRARIES}
${PROTOBUF_LIBRARIES}
${GLOG_LIBRARY}
gflags
${LIBGCRYPT_LIBRARIES}
${COREFOUNDATION_LIBRARY}
util
)

add_executable (ETClient terminal/TerminalClient.cpp)
target_link_libraries (
ETClient
LINK_PUBLIC
TerminalCommon
EternalTCP-static
${CMAKE_THREAD_LIBS_INIT}
${PROTOBUF_LIBRARIES}
${PROTOBUF_LIBRARIES}
${GLOG_LIBRARY}
gflags
${LIBGCRYPT_LIBRARIES}
${COREFOUNDATION_LIBRARY}
util
)

install(TARGETS ${PROJECT_NAME}-static DESTINATION lib)
#install(TARGETS ETClient ETServer DESTINATION bin)
install(FILES src/BackedWriter.hpp DESTINATION include/etcp)
#install(FILES launcher/ET DESTINATION bin)

if(BUILD_TEST) # Build unit tests.
  message(STATUS "Enabling test for ${PROJECT_NAME}")
  enable_testing()
  add_subdirectory(sandbox)
  add_subdirectory(test)
endif()


if(BUILD_PACKAGE)
  message(STATUS "Building Package for ${PROJECT_NAME}")

  set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
  set(CPACK_PACKAGE_VERSION_MAJOR ${${PROJECT_NAME}_VERSION_MAJOR}) 
  set(CPACK_PACKAGE_VERSION_MINOR ${${PROJECT_NAME}_VERSION_MINOR}) 
  set(CPACK_PACKAGE_VERSION_PATCH ${${PROJECT_NAME}_VERSION_PATCH})

  set(CPACK_PACKAGE_VERSION "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}")
  set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}")  
  set(CPACK_PACKAGE_VENDOR "Add Vendor") 
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Add Summary") 
  set(CPACK_PACKAGE_DESCRIPTION "Add a longer description") 
  set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_SOURCE_DIR}/README.md") 
  if(WIN32 AND NOT UNIX) 
    # for windows 
  elseif(APPLE) 
    # for mac
  else() 
    set(CPACK_GENERATOR "DEB") 
    # Strip files currently does not work, may be related to install TARGET
    set(CPACK_SOURCE_STRIP_FILES "")
    set(CPACK_STRIP_FILES "${CMAKE_BINARY_DIR}/${PROJECT_NAME}")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/")
    set(CPACK_DEBIAN_PACKAGE_SECTION "bin")
    # This will resolve dependencies automatically
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Jason G")
    install (TARGETS ETServer 
      PERMISSIONS  OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_WRITE GROUP_READ WORLD_EXECUTE WORLD_WRITE WORLD_READ
      DESTINATION "bin"
      )
    install (TARGETS ETClient
      PERMISSIONS  OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_WRITE GROUP_READ WORLD_EXECUTE WORLD_WRITE WORLD_READ
      DESTINATION "bin"
      )

    install(FILES launcher/ET 
      PERMISSIONS  OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_WRITE GROUP_READ WORLD_EXECUTE WORLD_WRITE WORLD_READ
      DESTINATION "bin")
    
    #To build and verify  
    #cpack && lintian -c EternalTCP-#.#.#-Linux.deb 
  endif()

  include(CPack)

endif()



