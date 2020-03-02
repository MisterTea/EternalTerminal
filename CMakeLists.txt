cmake_minimum_required (VERSION 3.0.2)
project (EternalTCP VERSION 6.0.7)

SET(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/external_imported/sanitizers-cmake/cmake" ${CMAKE_MODULE_PATH})
FIND_PACKAGE(Sanitizers)

SET(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/external_imported/cotire/CMake" ${CMAKE_MODULE_PATH})
INCLUDE(cotire)
if(POLICY CMP0058) 
    cmake_policy(SET CMP0058 NEW) # Needed for cotire
endif() 

option(CODE_COVERAGE "Enable code coverage" OFF)
option(DISABLE_CRASH_LOG "Disable installing easylogging crash handler" OFF)

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DET_VERSION='\"${PROJECT_VERSION}\"'")
# For easylogging, disable default log file, enable crash log, ensure thread safe, and catch c++ exceptions
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DELPP_NO_DEFAULT_LOG_FILE -DELPP_FEATURE_CRASH_LOG -DELPP_THREAD_SAFE")
IF(CODE_COVERAGE)
  if(UNIX)
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
  endif()
ENDIF(CODE_COVERAGE)

if(DISABLE_CRASH_LOG)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DELPP_DISABLE_DEFAULT_CRASH_HANDLING")
ENDIF(DISABLE_CRASH_LOG)

if(UNIX)
  # Enable C++-11
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -std=c++11")

  # Enable debug info
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
endif()

#Using FreeBSD?
if (CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
    set(FREEBSD TRUE)
endif (CMAKE_SYSTEM_NAME MATCHES "FreeBSD")

#Using NetBSD?
if (CMAKE_SYSTEM_NAME MATCHES "NetBSD")
  set(NETBSD TRUE)
endif (CMAKE_SYSTEM_NAME MATCHES "NetBSD")

# Add cmake script directory.
LIST(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Required packages
find_package(Threads REQUIRED)
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
  set(CORE_LIBRARIES util execinfo)
ELSEIF(NETBSD)
  set(CORE_LIBRARIES util resolv execinfo)
ELSE()
  set(CORE_LIBRARIES util resolv)
ENDIF()

MACRO(DECORATE_TARGET TARGET_NAME)
  add_sanitizers(${TARGET_NAME})
  set_target_properties(${TARGET_NAME} PROPERTIES COTIRE_CXX_PREFIX_HEADER_INIT "src/base/Headers.hpp")
  cotire(${TARGET_NAME})
ENDMACRO()

include_directories(
  external_imported/easyloggingpp/src
  external_imported/ThreadPool
  external_imported/Catch2/single_include
  external_imported/cxxopts/include
  external_imported/msgpack-c/include
  src/base
  src/terminal
  src/terminal/forwarding
  src/htm
  ${PROTOBUF_INCLUDE_DIRS}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CURSES_INCLUDE_DIR}
  ${sodium_INCLUDE_DIR}
  ${SELINUX_INCLUDE_DIR}
  ${UTEMPTER_INCLUDE_DIR}
  )

add_library(
  et-lib
  STATIC

  external_imported/easyloggingpp/src/easylogging++.h
  external_imported/easyloggingpp/src/easylogging++.cc

  src/base/BackedReader.hpp
  src/base/BackedReader.cpp

  src/base/BackedWriter.hpp
  src/base/BackedWriter.cpp

  src/base/ClientConnection.hpp
  src/base/ClientConnection.cpp

  src/base/Connection.hpp
  src/base/Connection.cpp

  src/base/CryptoHandler.hpp
  src/base/CryptoHandler.cpp

  src/base/ServerClientConnection.hpp
  src/base/ServerClientConnection.cpp

  src/base/ServerConnection.hpp
  src/base/ServerConnection.cpp

  src/base/SocketHandler.hpp
  src/base/SocketHandler.cpp

  src/base/PipeSocketHandler.hpp
  src/base/PipeSocketHandler.cpp

  src/base/TcpSocketHandler.hpp
  src/base/TcpSocketHandler.cpp

  src/base/UnixSocketHandler.hpp
  src/base/UnixSocketHandler.cpp

  src/base/LogHandler.hpp
  src/base/LogHandler.cpp

  src/base/DaemonCreator.hpp
  src/base/DaemonCreator.cpp

  src/base/SystemUtils.hpp
  src/base/SystemUtils.cpp

  src/base/RawSocketUtils.hpp
  src/base/RawSocketUtils.cpp

  ${ET_SRCS}
  )
add_dependencies(
  et-lib
  generated-code
  )
DECORATE_TARGET(et-lib)

add_library(
  TerminalCommon
  STATIC

  src/terminal/forwarding/PortForwardHandler.hpp
  src/terminal/forwarding/PortForwardHandler.cpp

  src/terminal/forwarding/ForwardSourceHandler.hpp
  src/terminal/forwarding/ForwardSourceHandler.cpp

  src/terminal/forwarding/ForwardDestinationHandler.hpp
  src/terminal/forwarding/ForwardDestinationHandler.cpp

  src/terminal/TerminalServer.hpp
  src/terminal/TerminalServer.cpp

  src/terminal/UserTerminalRouter.hpp
  src/terminal/UserTerminalRouter.cpp

  src/terminal/TerminalClient.hpp
  src/terminal/TerminalClient.cpp

  src/terminal/SshSetupHandler.hpp
  src/terminal/SshSetupHandler.cpp

  src/terminal/UserTerminalHandler.hpp
  src/terminal/UserTerminalHandler.cpp

  src/terminal/UserJumphostHandler.hpp
  src/terminal/UserJumphostHandler.cpp

  ${ETERMINAL_SRCS} ${ETERMINAL_HDRS}
  )
add_dependencies(
  TerminalCommon
  generated-code
  )
DECORATE_TARGET(TerminalCommon)

add_executable (
  etserver
  src/terminal/TerminalServerMain.cpp
  )
target_link_libraries (
  etserver
  LINK_PUBLIC
  TerminalCommon
  et-lib
  ${CMAKE_THREAD_LIBS_INIT}
  ${PROTOBUF_LITE_LIBRARIES}
  ${sodium_LIBRARY_RELEASE}
  ${SELINUX_LIBRARIES}
  ${UTEMPTER_LIBRARIES}
  ${CORE_LIBRARIES}
)
DECORATE_TARGET(etserver)

add_executable (
  etterminal
  src/terminal/TerminalMain.cpp
  )
target_link_libraries (
  etterminal
  LINK_PUBLIC
  TerminalCommon
  et-lib
  ${CMAKE_THREAD_LIBS_INIT}
  ${PROTOBUF_LITE_LIBRARIES}
  ${sodium_LIBRARY_RELEASE}
  ${SELINUX_LIBRARIES}
  ${UTEMPTER_LIBRARIES}
  ${CORE_LIBRARIES}
)
DECORATE_TARGET(etterminal)

add_executable (
  et
  src/terminal/TerminalClientMain.cpp
  )
target_link_libraries (
    et
    LINK_PUBLIC
    TerminalCommon
    et-lib
    ${CMAKE_THREAD_LIBS_INIT}
    ${PROTOBUF_LITE_LIBRARIES}
    ${sodium_LIBRARY_RELEASE}
    ${UTEMPTER_LIBRARIES}
    ${CORE_LIBRARIES}
)
DECORATE_TARGET(et)

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
DECORATE_TARGET(HtmCommon)

add_executable (
  htm

  src/htm/HtmClientMain.cpp
  )
target_link_libraries (
  htm
  LINK_PUBLIC

  HtmCommon
  et-lib
  ${CMAKE_THREAD_LIBS_INIT}
  ${PROTOBUF_LITE_LIBRARIES}
  ${sodium_LIBRARY_RELEASE}
  ${SELINUX_LIBRARIES}
  ${UTEMPTER_LIBRARIES}
  ${CORE_LIBRARIES}
)
DECORATE_TARGET(htm)

add_executable (
  htmd

  src/htm/HtmServerMain.cpp
  )
target_link_libraries (
  htmd
  LINK_PUBLIC

  HtmCommon
  et-lib
  ${CMAKE_THREAD_LIBS_INIT}
  ${PROTOBUF_LITE_LIBRARIES}
  ${sodium_LIBRARY_RELEASE}
  ${SELINUX_LIBRARIES}
  ${UTEMPTER_LIBRARIES}
  ${CORE_LIBRARIES}
)
DECORATE_TARGET(htmd)

enable_testing()

file(GLOB TEST_SRCS test/*Test.cpp )
add_executable(
  et-test

  ${TEST_SRCS}
  test/Main.cpp
  )
add_dependencies(
  et-test
  TerminalCommon
  et-lib)
target_link_libraries(
  et-test
  TerminalCommon
  et-lib
  ${CMAKE_THREAD_LIBS_INIT}
  ${PROTOBUF_LITE_LIBRARIES}
  ${sodium_LIBRARY_RELEASE}
  ${SELINUX_LIBRARIES}
  ${UTEMPTER_LIBRARIES}
  ${CORE_LIBRARIES}
  )
add_test(
  et-test
  et-test
  )
DECORATE_TARGET(et-test)

install(TARGETS etserver etterminal et htm htmd
  PERMISSIONS  OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ
  DESTINATION "bin"
  )
