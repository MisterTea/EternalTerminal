cmake_minimum_required(VERSION 3.0.2)

set(EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/external")

if(DISABLE_VCPKG)
else()
  # Enable vcpkg
  set(CMAKE_TOOLCHAIN_FILE
      "${EXTERNAL_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")

  # Set the triplet
  if(WIN32)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static")
  elseif(APPLE)
    execute_process(
      COMMAND uname -m
      COMMAND tr -d '\n'
      OUTPUT_VARIABLE ARCHITECTURE)
    message(STATUS "Architecture: ${ARCHITECTURE}")
    if(${ARCHITECTURE} STREQUAL "arm64") # Apple Silicon
      set(VCPKG_TARGET_TRIPLET "arm64-osx")
    else()
      set(VCPKG_TARGET_TRIPLET "x64-osx")
    endif()
  else()
    execute_process(
      COMMAND uname -m
      COMMAND tr -d '\n'
      OUTPUT_VARIABLE ARCHITECTURE)
    message(STATUS "Architecture: ${ARCHITECTURE}")
    if(${ARCHITECTURE} STREQUAL "x86_64")
      set(VCPKG_TARGET_TRIPLET "x64-linux")
    elseif(${ARCHITECTURE} STREQUAL "x86_86")
      set(VCPKG_TARGET_TRIPLET "x86-linux")
    elseif(${ARCHITECTURE} STREQUAL "aarch64")
      set(VCPKG_TARGET_TRIPLET "arm64-linux")
    elseif(${ARCHITECTURE} STREQUAL "arm64")
      set(VCPKG_TARGET_TRIPLET "arm64-linux")
    elseif(${ARCHITECTURE} MATCHES "arm.*")
      set(VCPKG_TARGET_TRIPLET "arm-linux")
    else()
      set(VCPKG_TARGET_TRIPLET "${ARCHITECTURE}-linux")
    endif()
  endif()

  # Some hacks to fix vcpkg
  if(WIN32)
    set(Protobuf_PROTOC_EXECUTABLE
        "${EXTERNAL_DIR}/vcpkg/packages/protobuf_${VCPKG_TARGET_TRIPLET}/tools/protobuf/protoc.exe"
    )
    set(ZLIB_LIBRARY_RELEASE
        "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib/zlib.lib"
    )
    set(ZLIB_LIBRARY_DEBUG
        "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/debug/lib/zlibd.lib"
    )
  else()
    set(Protobuf_PROTOC_EXECUTABLE
        "${EXTERNAL_DIR}/vcpkg/packages/protobuf_${VCPKG_TARGET_TRIPLET}/tools/protobuf/protoc"
    )
  endif()
  set(Protobuf_USE_STATIC_LIBS ON)
  set(protobuf_MSVC_STATIC_RUNTIME ON)
endif()

# Don't specify languages yet in case we need to bump the cmake version
project(EternalTCP VERSION 6.1.7 LANGUAGES NONE)

if(WIN32)
message(STATUS "Windows detected.  Bumping up cmake version.")
# Hack to force a higher minimum version on windows
cmake_minimum_required(VERSION 3.15.0)
endif()

enable_language(C)
enable_language(CXX)

include(CMakeFindDependencyMacro)

# Add cmake script directory.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
set(CMAKE_MODULE_PATH "${EXTERNAL_DIR}/sanitizers-cmake/cmake"
                      ${CMAKE_MODULE_PATH})

# Required packages
find_package(OpenSSL REQUIRED)
find_package(Sanitizers REQUIRED)
find_package(Threads REQUIRED)
find_package(sodium REQUIRED)
find_package(Protobuf REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Unwind)

# Optional packages
find_package(UTempter)
if(LINUX)
  find_package(SELinux)
endif()

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Using FreeBSD?
if(CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
  set(FREEBSD TRUE)
endif(CMAKE_SYSTEM_NAME MATCHES "FreeBSD")

# Using NetBSD?
if(CMAKE_SYSTEM_NAME MATCHES "NetBSD")
  set(NETBSD TRUE)
endif(CMAKE_SYSTEM_NAME MATCHES "NetBSD")

if(FREEBSD OR NETBSD OR DISABLE_SENTRY)
  # Sentry doesn't work on BSD
else()
  # Enable sentry
  set(USE_SENTRY TRUE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DUSE_SENTRY")
endif()

if(USE_SENTRY)
set(SENTRY_BUILD_RUNTIMESTATIC ON)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory("${EXTERNAL_DIR}/sentry-native")
endif()

set(CMAKE_MODULE_PATH "${EXTERNAL_DIR}/cotire/CMake"
                      ${CMAKE_MODULE_PATH})
include(cotire)
if(POLICY CMP0058)
  cmake_policy(SET CMP0058 NEW) # Needed for cotire
endif()

option(CODE_COVERAGE "Enable code coverage" OFF)
option(DISABLE_CRASH_LOG "Disable installing easylogging crash handler" OFF)

add_definitions(-DET_VERSION="${PROJECT_VERSION}")
# For easylogging, disable default log file, enable crash log, ensure thread
# safe, and catch c++ exceptions
set(CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} -DELPP_NO_DEFAULT_LOG_FILE -DELPP_FEATURE_CRASH_LOG -DELPP_THREAD_SAFE -DSENTRY_BUILD_STATIC"
)

if(CODE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g --coverage")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g --coverage")
endif(CODE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")

if(DISABLE_CRASH_LOG)
  set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} -DELPP_DISABLE_DEFAULT_CRASH_HANDLING")
endif(DISABLE_CRASH_LOG)

if(UNIX)
  # Enable debug info
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -ggdb3")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -ggdb3")
endif()

# Enable C++-17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
  # Enable unicode
  add_definitions(-DUNICODE)
  add_definitions(-D_UNICODE)
endif()

option(FULL_PROTOBUF "Link to full protobuf library instead of protobuf-lite"
       OFF)
if(FULL_PROTOBUF)
  if(WIN32)
    set(PROTOBUF_LIBS protobuf::libprotobuf)
  else()
    set(PROTOBUF_LIBS ${PROTOBUF_LIBRARIES})
  endif()
else()
  if(WIN32)
    set(PROTOBUF_LIBS protobuf::libprotobuf-lite)
  else()
    set(PROTOBUF_LIBS ${PROTOBUF_LITE_LIBRARIES})
  endif()
endif()

if(SELINUX_FOUND)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DWITH_SELINUX")
else()
  set(SELINUX_INCLUDE_DIR "")
  set(SELINUX_LIBRARIES "")
endif()

if(UTEMPTER_FOUND)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DWITH_UTEMPTER")
else()
  set(UTEMPTER_INCLUDE_DIR "")
  set(UTEMPTER_LIBRARIES "")
endif()

protobuf_generate_cpp(ET_SRCS ET_HDRS proto/ET.proto)
set_source_files_properties(${ET_SRCS} ${ET_HDRS} PROPERTIES GENERATED TRUE)
protobuf_generate_cpp(ETERMINAL_SRCS ETERMINAL_HDRS proto/ETerminal.proto)
set_source_files_properties(${ETERMINAL_SRCS} ${ETERMINAL_HDRS}
                            PROPERTIES GENERATED TRUE)
add_custom_target(generated-code DEPENDS ${ET_SRCS} ${ET_HDRS}
                                         ${ETERMINAL_SRCS} ${ETERMINAL_HDRS})

if(ANDROID)
  set(CORE_LIBRARIES OpenSSL::SSL ZLIB::ZLIB util)
elseif(FREEBSD)
  set(CORE_LIBRARIES OpenSSL::SSL ZLIB::ZLIB util execinfo)
elseif(NETBSD)
  set(CORE_LIBRARIES OpenSSL::SSL ZLIB::ZLIB util resolv
                     execinfo)
elseif(WIN32)
  set(CORE_LIBRARIES OpenSSL::SSL ZLIB::ZLIB Ws2_32 Shlwapi
                     dbghelp)
elseif(APPLE)
  set(CORE_LIBRARIES OpenSSL::SSL ZLIB::ZLIB util resolv)
else()
  set(CORE_LIBRARIES
      OpenSSL::SSL
      ZLIB::ZLIB
      util
      resolv
      atomic
      stdc++fs)
endif()

IF(Unwind_FOUND)
  list(INSERT CORE_LIBRARIES 0 unwind::unwind)
ENDIF()

IF(USE_SENTRY)
  list(INSERT CORE_LIBRARIES 0 sentry::sentry)
ENDIF()

macro(DECORATE_TARGET TARGET_NAME)
  add_sanitizers(${TARGET_NAME})
  set_target_properties(${TARGET_NAME} PROPERTIES COTIRE_CXX_PREFIX_HEADER_INIT
                                                  "src/base/Headers.hpp")
  if(CMAKE_CROSSCOMPILING)
    # Doesn't work when cross-compiling
  else()
    cotire(${TARGET_NAME})
  endif()
endmacro()

include_directories(
  ${EXTERNAL_DIR}/easyloggingpp/src
  ${EXTERNAL_DIR}/ThreadPool
  ${EXTERNAL_DIR}/PlatformFolders
  ${EXTERNAL_DIR}/Catch2/single_include
  ${EXTERNAL_DIR}/cxxopts/include
  ${EXTERNAL_DIR}/msgpack-c/include
  ${EXTERNAL_DIR}/sentry-native/include
  ${EXTERNAL_DIR}/json/single_include/nlohmann
  ${EXTERNAL_DIR}/sole
  ${EXTERNAL_DIR}/base64
  ${EXTERNAL_DIR}/simpleini
  ${EXTERNAL_DIR}/cpp-httplib
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
  ${OPENSSL_INCLUDE_DIR})

if(NOT ANDROID)
  include_directories(${EXTERNAL_DIR}/UniversalStacktrace/ust)
endif()

add_library(
  et-lib STATIC

  ${EXTERNAL_DIR}/easyloggingpp/src/easylogging++.h
  ${EXTERNAL_DIR}/easyloggingpp/src/easylogging++.cc
  ${EXTERNAL_DIR}/PlatformFolders/sago/platform_folders.cpp

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
  src/base/RawSocketUtils.hpp
  src/base/RawSocketUtils.cpp
  src/base/WinsockContext.hpp
  src/base/SubprocessToString.hpp
  src/base/SubprocessToString.cpp
  ${ET_SRCS})
add_dependencies(et-lib generated-code)
decorate_target(et-lib)

add_library(
  TerminalCommon STATIC
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
  src/terminal/TelemetryService.hpp
  src/terminal/TelemetryService.cpp
  ${ETERMINAL_SRCS}
  ${ETERMINAL_HDRS})
add_dependencies(TerminalCommon generated-code)
decorate_target(TerminalCommon)

add_executable(et src/terminal/TerminalClientMain.cpp)
target_link_libraries(
  et
  LINK_PUBLIC
  TerminalCommon
  et-lib
  ${CMAKE_THREAD_LIBS_INIT}
  ${PROTOBUF_LIBS}
  ${sodium_LIBRARY_RELEASE}
  ${UTEMPTER_LIBRARIES}
  ${CORE_LIBRARIES})
decorate_target(et)

if(WIN32)
  install(
    TARGETS et
    DESTINATION "bin"
    COMPONENT client)
  set(CPACK_COMPONENTS_ALL client)
  set(CPACK_COMPONENT_CLIENT_DISPLAY_NAME "Eternal Terminal Client")

else(WIN32)
  add_executable(etserver src/terminal/TerminalServerMain.cpp)
  target_link_libraries(
    etserver
    LINK_PUBLIC
    TerminalCommon
    et-lib
    ${CMAKE_THREAD_LIBS_INIT}
    ${PROTOBUF_LIBS}
    ${sodium_LIBRARY_RELEASE}
    ${SELINUX_LIBRARIES}
    ${UTEMPTER_LIBRARIES}
      ${CORE_LIBRARIES})
  decorate_target(etserver)

  add_executable(etterminal src/terminal/TerminalMain.cpp)
  target_link_libraries(
    etterminal
    LINK_PUBLIC
    TerminalCommon
    et-lib
    ${CMAKE_THREAD_LIBS_INIT}
    ${PROTOBUF_LIBS}
    ${sodium_LIBRARY_RELEASE}
    ${SELINUX_LIBRARIES}
    ${UTEMPTER_LIBRARIES}
      ${CORE_LIBRARIES})
  decorate_target(etterminal)

  add_library(
    HtmCommon STATIC
    src/htm/TerminalHandler.cpp
    src/htm/MultiplexerState.cpp
    src/htm/IpcPairClient.cpp
    src/htm/IpcPairEndpoint.cpp
    src/htm/IpcPairServer.cpp
    src/htm/HtmClient.cpp
    src/htm/HtmServer.cpp)
  add_dependencies(HtmCommon generated-code)
  decorate_target(HtmCommon)

  add_executable(htm src/htm/HtmClientMain.cpp)
  target_link_libraries(
    htm
    LINK_PUBLIC
    HtmCommon
    et-lib
    ${CMAKE_THREAD_LIBS_INIT}
    ${PROTOBUF_LIBS}
    ${sodium_LIBRARY_RELEASE}
    ${SELINUX_LIBRARIES}
    ${UTEMPTER_LIBRARIES}
      ${CORE_LIBRARIES})
  decorate_target(htm)

  add_executable(htmd src/htm/HtmServerMain.cpp)
  target_link_libraries(
    htmd
    LINK_PUBLIC
    HtmCommon
    et-lib
    ${CMAKE_THREAD_LIBS_INIT}
    ${PROTOBUF_LIBS}
    ${sodium_LIBRARY_RELEASE}
    ${SELINUX_LIBRARIES}
    ${UTEMPTER_LIBRARIES}
      ${CORE_LIBRARIES})
  decorate_target(htmd)

  enable_testing()

  file(GLOB TEST_SRCS test/*Test.cpp)
  add_executable(et-test ${TEST_SRCS} test/Main.cpp)
  add_dependencies(et-test TerminalCommon et-lib)
  target_link_libraries(
    et-test
    TerminalCommon
    et-lib
    ${CMAKE_THREAD_LIBS_INIT}
    ${PROTOBUF_LIBS}
    ${sodium_LIBRARY_RELEASE}
    ${SELINUX_LIBRARIES}
    ${UTEMPTER_LIBRARIES}
      ${CORE_LIBRARIES})
  add_test(et-test et-test)
  decorate_target(et-test)

  install(
    TARGETS etserver etterminal et htm htmd
    PERMISSIONS
      OWNER_EXECUTE
      OWNER_WRITE
      OWNER_READ
      GROUP_EXECUTE
      GROUP_READ
      WORLD_EXECUTE
      WORLD_READ
    DESTINATION "bin")
endif()

set(CPACK_PACKAGE_NAME "Eternal Terminal")
set(CPACK_PACKAGE_VENDOR "https://github.com/MisterTea/EternalTerminal")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Remote terminal for the busy and impatient")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "EternalTerminal")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

# This must always be last!
include(CPack)
