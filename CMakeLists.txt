cmake_minimum_required (VERSION 3.0.2)
project (EternalTCP VERSION 6.0.13)

SET(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/external_imported/sanitizers-cmake/cmake" ${CMAKE_MODULE_PATH})
FIND_PACKAGE(Sanitizers)

SET(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/external_imported/cotire/CMake" ${CMAKE_MODULE_PATH})
INCLUDE(cotire)
if(POLICY CMP0058) 
    cmake_policy(SET CMP0058 NEW) # Needed for cotire
endif() 

option(CODE_COVERAGE "Enable code coverage" OFF)
option(DISABLE_CRASH_LOG "Disable installing easylogging crash handler" OFF)

add_definitions(-DET_VERSION="${PROJECT_VERSION}")
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
  # Enable debug info
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -ggdb3")
  SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -ggdb3")
endif()

# Enable C++-11
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
# Enable unicode
ADD_DEFINITIONS(-DUNICODE)
ADD_DEFINITIONS(-D_UNICODE)
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
find_package(sodium REQUIRED)

# Optional packages
IF(LINUX)
find_package(SELinux)
ENDIF()
find_package(UTempter)

option(FULL_PROTOBUF "Link to full protobuf library instead of protobuf-lite" OFF)
IF(FULL_PROTOBUF)
    SET(PROTOBUF_LIBS ${PROTOBUF_LIBRARIES})
ELSE()
    SET(PROTOBUF_LIBS ${PROTOBUF_LITE_LIBRARIES})
ENDIF()

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
ELSEIF(WIN32)
  set(CORE_LIBRARIES Ws2_32 Shlwapi dbghelp)
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
  external_imported/UniversalStacktrace/ust
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

  src/base/WinsockContext.hpp

  src/base/SubprocessToString.hpp
  src/base/SubprocessToString.cpp

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
  et
  src/terminal/TerminalClientMain.cpp
  )
target_link_libraries (
    et
    LINK_PUBLIC
    TerminalCommon
    et-lib
    ${CMAKE_THREAD_LIBS_INIT}
    ${PROTOBUF_LIBS}
    ${sodium_LIBRARY_RELEASE}
    ${UTEMPTER_LIBRARIES}
    ${CORE_LIBRARIES}
)
DECORATE_TARGET(et)

IF(WIN32)
install(TARGETS et
  DESTINATION "bin"
  COMPONENT client
  )
set(CPACK_COMPONENTS_ALL client)
set(CPACK_COMPONENT_CLIENT_DISPLAY_NAME "Eternal Terminal Client")
  
ELSE(WIN32)
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
  ${PROTOBUF_LIBS}
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
  ${PROTOBUF_LIBS}
  ${sodium_LIBRARY_RELEASE}
  ${SELINUX_LIBRARIES}
  ${UTEMPTER_LIBRARIES}
  ${CORE_LIBRARIES}
)
DECORATE_TARGET(etterminal)

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
  ${PROTOBUF_LIBS}
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
  ${PROTOBUF_LIBS}
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
  ${PROTOBUF_LIBS}
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
ENDIF()

set(CPACK_PACKAGE_NAME "Eternal Terminal")
set(CPACK_PACKAGE_VENDOR "https://github.com/MisterTea/EternalTerminal")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Remote terminal for the busy and impatient")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "EternalTerminal")
SET(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

# This must always be last!
include(CPack)
