cmake_minimum_required (VERSION 2.8.12)
project (EternalTCP)

option(BUILD_TEST "Build all unit test" OFF)


set (${PROJECT_NAME}_VERSION_MAJOR 5)
set (${PROJECT_NAME}_VERSION_MINOR 0)
set (${PROJECT_NAME}_VERSION_PATCH 6)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DET_VERSION='\"${EternalTCP_VERSION_MAJOR}.${EternalTCP_VERSION_MINOR}.${EternalTCP_VERSION_PATCH}\"'")
# For easylogging, disable default log file, enable crash log, ensure thread safe, and catch c++ exceptions
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DELPP_NO_DEFAULT_LOG_FILE -DELPP_FEATURE_CRASH_LOG -DELPP_THREAD_SAFE -DELPP_HANDLE_SIGABRT")
IF(BUILD_TEST)
  add_subdirectory(${CMAKE_SOURCE_DIR}/external/googletest)
  if(UNIX)
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
  endif()
ENDIF(BUILD_TEST)

# Enable C++-11
if(UNIX)
	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -std=c++11")
endif()

#Using FreeBSD?
if (CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
    set(FREEBSD TRUE)
endif (CMAKE_SYSTEM_NAME MATCHES "FreeBSD")

# Add cmake script directory.
LIST(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Required packages
find_package(Threads REQUIRED)
find_package(GFlags REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Sodium REQUIRED)

# Optional packages
IF(LINUX)
find_package(SELinux)
ENDIF()
find_package(UTempter)

IF(SELINUX_FOUND)
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DWITH_SELINUX")
ELSE()
  SET(SELINUX_INCLUDE_DIR "")
  SET(SELINUX_LIBRARIES "")
ENDIF()

IF(UTEMPTER_FOUND)
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DWITH_UTEMPTER")
ELSE()
  SET(UTEMPTER_INCLUDE_DIR "")
  SET(UTEMPTER_LIBRARIES "")
ENDIF()

PROTOBUF_GENERATE_CPP(
  ET_SRCS
  ET_HDRS

  proto/ET.proto
  )
PROTOBUF_GENERATE_CPP(
  ETERMINAL_SRCS
  ETERMINAL_HDRS

  proto/ETerminal.proto
  )
add_custom_target(
  generated-code
  DEPENDS

  ${ET_SRCS} ${ET_HDRS}
  ${ETERMINAL_SRCS} ${ETERMINAL_HDRS}
  )

IF(FREEBSD)
  set(CORE_LIBRARIES util)
ELSE()
  set(CORE_LIBRARIES util resolv)
ENDIF()

include_directories(
  src/base
  src/terminal
  src/htm
  ${PROTOBUF_INCLUDE_DIRS}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CURSES_INCLUDE_DIR}
  ${sodium_INCLUDE_DIR}
  ${GFLAGS_INCLUDE_DIRS}
  ${SELINUX_INCLUDE_DIR}
  ${UTEMPTER_INCLUDE_DIR}
  )

add_library(
  et-lib
  STATIC

  src/base/BackedReader.cpp
  src/base/BackedWriter.cpp
  src/base/ClientConnection.cpp
  src/base/Connection.cpp
  src/base/CryptoHandler.cpp
  src/base/FakeSocketHandler.cpp
  src/base/FlakyFakeSocketHandler.cpp
  src/base/ServerClientConnection.cpp
  src/base/ServerConnection.cpp
  src/base/SocketHandler.cpp
  src/base/UnixSocketHandler.cpp
  src/base/easylogging++.cc

  ${ET_SRCS}
  )
add_dependencies(
  et-lib
  generated-code
  )

add_library(
  TerminalCommon
  STATIC

  src/terminal/DaemonCreator.cpp
  src/terminal/SystemUtils.cpp
  src/terminal/RawSocketUtils.cpp
  src/terminal/PortForwardHandler.cpp
  src/terminal/PortForwardSourceHandler.cpp
  src/terminal/PortForwardDestinationHandler.cpp
  src/terminal/LogHandler.cpp

  ${ETERMINAL_SRCS} ${ETERMINAL_HDRS}
  )
add_dependencies(
  TerminalCommon
  generated-code
  )

add_executable (
  etserver
  src/terminal/TerminalServer.cpp
  src/terminal/UserTerminalHandler.cpp
  src/terminal/UserTerminalRouter.cpp
  )

target_link_libraries (
  etserver
  LINK_PUBLIC
  TerminalCommon
  et-lib
  ${CMAKE_THREAD_LIBS_INIT}
  ${PROTOBUF_LIBRARIES}
  ${GFLAGS_LIBRARIES}
  ${sodium_LIBRARY_RELEASE}
  ${SELINUX_LIBRARIES}
  ${UTEMPTER_LIBRARIES}
  ${CORE_LIBRARIES}
)

add_executable (
  et
  src/terminal/TerminalClient.cpp
  src/terminal/SshSetupHandler.cpp
  )

target_link_libraries (
    et
    LINK_PUBLIC
    TerminalCommon
    et-lib
    ${CMAKE_THREAD_LIBS_INIT}
    ${PROTOBUF_LIBRARIES}
    ${GFLAGS_LIBRARIES}
    ${sodium_LIBRARY_RELEASE}
    ${UTEMPTER_LIBRARIES}
    ${CORE_LIBRARIES}
)

add_library (
  HtmCommon
  STATIC

  src/htm/TerminalHandler.cpp
  src/htm/MultiplexerState.cpp
  src/htm/IpcPairClient.cpp
  src/htm/IpcPairEndpoint.cpp
  src/htm/IpcPairServer.cpp
  src/htm/HtmClient.cpp
  src/htm/HtmServer.cpp
)
add_dependencies(
  HtmCommon
  generated-code
  )

add_executable (
  htm

  src/htm/HtmClientMain.cpp
  )

target_link_libraries (
  htm
  LINK_PUBLIC

  HtmCommon
  TerminalCommon
  et-lib
  ${CMAKE_THREAD_LIBS_INIT}
  ${PROTOBUF_LIBRARIES}
  ${GLOG_LIBRARIES}
  ${GFLAGS_LIBRARIES}
  ${sodium_LIBRARY_RELEASE}
  ${SELINUX_LIBRARIES}
  ${UTEMPTER_LIBRARIES}
  ${CORE_LIBRARIES}
)

add_executable (
  htmd

  src/htm/HtmServerMain.cpp
  )

target_link_libraries (
  htmd
  LINK_PUBLIC

  HtmCommon
  TerminalCommon
  et-lib
  ${CMAKE_THREAD_LIBS_INIT}
  ${PROTOBUF_LIBRARIES}
  ${GLOG_LIBRARIES}
  ${GFLAGS_LIBRARIES}
  ${sodium_LIBRARY_RELEASE}
  ${SELINUX_LIBRARIES}
  ${UTEMPTER_LIBRARIES}
  ${CORE_LIBRARIES}
)

if(BUILD_TEST) # Build unit tests.
  message(STATUS "Enabling test for ${PROJECT_NAME}")
  enable_testing()
  add_subdirectory(sandbox)
  add_subdirectory(test)
endif()

install(TARGETS etserver et htm htmd
  PERMISSIONS  OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ
  DESTINATION "bin"
  )
