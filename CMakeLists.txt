cmake_minimum_required(VERSION 3.0.2)

set(EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/external")

if(DISABLE_VCPKG)
else()
  # Enable vcpkg
  set(CMAKE_TOOLCHAIN_FILE
      "${EXTERNAL_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")

  # Set the triplet
  if(WIN32)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "VCPKG Triplet")
  elseif(APPLE)
    execute_process(
      COMMAND uname -m
      COMMAND tr -d '\n'
      OUTPUT_VARIABLE VCPKG_ARCHITECTURE)
    message(STATUS "Architecture: ${VCPKG_ARCHITECTURE}")
    if(${VCPKG_ARCHITECTURE} STREQUAL "arm64") # Apple Silicon
      set(VCPKG_TARGET_TRIPLET "arm64-osx" CACHE STRING "VCPKG Triplet")
    else()
      set(VCPKG_TARGET_TRIPLET "x64-osx" CACHE STRING "VCPKG Triplet")
    endif()
  else()
    execute_process(
      COMMAND uname -m
      COMMAND tr -d '\n'
      OUTPUT_VARIABLE VCPKG_ARCHITECTURE)
    message(STATUS "Architecture: ${VCPKG_ARCHITECTURE}")
    if(${VCPKG_ARCHITECTURE} STREQUAL "x86_64")
      set(VCPKG_TARGET_TRIPLET "x64-linux" CACHE STRING "VCPKG Triplet")
    elseif(${VCPKG_ARCHITECTURE} STREQUAL "x86_86")
      set(VCPKG_TARGET_TRIPLET "x86-linux" CACHE STRING "VCPKG Triplet")
    elseif(${VCPKG_ARCHITECTURE} STREQUAL "aarch64")
      set(VCPKG_TARGET_TRIPLET "arm64-linux" CACHE STRING "VCPKG Triplet")
    elseif(${VCPKG_ARCHITECTURE} STREQUAL "arm64")
      set(VCPKG_TARGET_TRIPLET "arm64-linux" CACHE STRING "VCPKG Triplet")
    elseif(${VCPKG_ARCHITECTURE} MATCHES "arm.*")
      set(VCPKG_TARGET_TRIPLET "arm-linux" CACHE STRING "VCPKG Triplet")
    else()
      set(VCPKG_TARGET_TRIPLET "${VCPKG_ARCHITECTURE}-linux" CACHE STRING "VCPKG Triplet")
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
    # Make BoostConfig.cmake able to check its inputs
    cmake_policy(SET CMP0057 NEW)
    # Honor the visibility properties for all target types, including
    # object libraries and static libraries.
    cmake_policy(SET CMP0063 NEW)
    # Don't ignore <PackageName>_ROOT variables
    cmake_policy(SET CMP0074 NEW)
    set(Protobuf_PROTOC_EXECUTABLE
        "${EXTERNAL_DIR}/vcpkg/packages/protobuf_${VCPKG_TARGET_TRIPLET}/tools/protobuf/protoc"
    )
  endif()
  set(Protobuf_USE_STATIC_LIBS ON)
  set(protobuf_MSVC_STATIC_RUNTIME ON)
endif()

message(STATUS "VCPKG TRIPLET: ${VCPKG_TARGET_TRIPLET}")

# Don't specify languages yet in case we need to bump the cmake version
project(EternalTCP VERSION 6.2.1 LANGUAGES NONE)

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

option(DISABLE_SENTRY "Disable Sentry crash logging" OFF)
option(DISABLE_TELEMETRY "Disable all crash logging" OFF)

IF(APPLE)
  # Proxy test for Mojave? (Clang that doesn't have std::filesystem)
  IF(CMAKE_HOST_SYSTEM_VERSION VERSION_LESS "19.0.0")
    # So use Boost's version
    find_package(Boost 1.76 COMPONENTS filesystem REQUIRED)
  ENDIF()
ENDIF(APPLE)

if(FREEBSD OR NETBSD OR DISABLE_SENTRY OR DISABLE_TELEMETRY)
  # Sentry doesn't work on BSD
else()
  # Enable sentry
  set(USE_SENTRY TRUE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DUSE_SENTRY")
endif()

if(DISABLE_TELEMETRY)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DNO_TELEMETRY")
endif()

if(USE_SENTRY)
if(DISABLE_VCPKG)
set(SENTRY_BUILD_RUNTIMESTATIC ON)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory("${EXTERNAL_DIR}/sentry-native")
include_directories(
  ${EXTERNAL_DIR}/sentry-native/include
)
else()
find_package(sentry CONFIG REQUIRED)
endif()
endif()

set(CMAKE_MODULE_PATH "${EXTERNAL_DIR}/cotire/CMake"
                      ${CMAKE_MODULE_PATH})
include(cotire)
if(POLICY CMP0058)
  cmake_policy(SET CMP0058 NEW) # Needed for cotire
endif()

option(CODE_COVERAGE "Enable code coverage" OFF)
option(FUZZING "Enable builds for fuzz testing" OFF)
option(DISABLE_CRASH_LOG "Disable installing easylogging crash handler" OFF)

add_definitions(-DET_VERSION="${PROJECT_VERSION}")
# For easylogging, disable default log file, enable crash log, ensure thread
# safe, and catch c++ exceptions
set(CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} -DELPP_NO_DEFAULT_LOG_FILE -DELPP_FEATURE_CRASH_LOG -DELPP_THREAD_SAFE -DELPP_STRICT_PERMISSIONS -DSENTRY_BUILD_STATIC"
)
IF(WIN32)
  SET(CMAKE_CXX_FLAGS "-DSODIUM_STATIC")
ENDIF(WIN32)

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

# Enable compile commands export for vscode
add_definitions(-DCMAKE_EXPORT_COMPILE_COMMANDS=ON)

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

if(NOT Boost_FOUND)
  set(Boost_INCLUDE_DIR "")
  set(Boost_LIBRARIES "")
endif()

IF(NOT APPLE AND NOT WIN32)
  if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    SET(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)
  endif()
ENDIF()

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

macro(DECORATE_FUZZER TARGET_NAME)
  add_sanitizers(${TARGET_NAME})

  if(FUZZING)
    # ASAN must also be enabled to build fuzzers.
    if(NOT SANITIZE_ADDRESS)
      message(FATAL_ERROR "Fuzzing requires SANITIZE_ADDRESS=ON to detect memory errors.")
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      set_property(TARGET ${TARGET_NAME} APPEND_STRING
          PROPERTY COMPILE_FLAGS " -fsanitize=fuzzer")
      set_property(TARGET ${TARGET_NAME} APPEND_STRING
          PROPERTY LINK_FLAGS " -fsanitize=fuzzer")
    else()
      message(FATAL_ERROR "Currently fuzzing is only supported with Clang.")
    endif()
  endif()
endmacro()

include_directories(
  ${EXTERNAL_DIR}/easyloggingpp/src
  ${EXTERNAL_DIR}/ThreadPool
  ${EXTERNAL_DIR}/PlatformFolders
  ${EXTERNAL_DIR}/Catch2/single_include
  ${EXTERNAL_DIR}/cxxopts/include
  ${EXTERNAL_DIR}/msgpack-c/include
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
  ${Boost_INCLUDE_DIR}
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
  src/base/TunnelUtils.hpp
  src/base/TunnelUtils.cpp

  ${ET_HDRS}
  ${ET_SRCS}
)
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
  src/terminal/ServerFifoPath.hpp
  src/terminal/ServerFifoPath.cpp
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
add_dependencies(TerminalCommon generated-code et-lib)
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
  ${Boost_LIBRARIES}
  ${CORE_LIBRARIES})
add_dependencies(et generated-code et-lib)
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
    ${Boost_LIBRARIES}
      ${CORE_LIBRARIES})
  add_dependencies(etserver generated-code et-lib)
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
    ${Boost_LIBRARIES}
      ${CORE_LIBRARIES})
  add_dependencies(etterminal generated-code et-lib)
  decorate_target(etterminal)

  add_library(
    HtmCommon STATIC
    src/htm/TerminalHandler.cpp
    src/htm/MultiplexerState.cpp
    src/htm/IpcPairClient.cpp
    src/htm/IpcPairEndpoint.cpp
    src/htm/IpcPairServer.cpp
    src/htm/HtmClient.cpp
    src/htm/HtmServer.cpp
  )
  add_dependencies(HtmCommon generated-code et-lib)
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
    ${Boost_LIBRARIES}
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
    ${Boost_LIBRARIES}
      ${CORE_LIBRARIES})
  decorate_target(htmd)

  enable_testing()

  file(GLOB TEST_SRCS test/*Test.cpp)
  add_executable(
  et-test
  ${TEST_SRCS}
  test/Main.cpp
  )
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
    ${Boost_LIBRARIES}
      ${CORE_LIBRARIES})
  add_test(et-test et-test)
  decorate_target(et-test)

  if(FUZZING)
    add_executable(
    TerminalServerFuzzer
    test/TerminalServerFuzzer.cpp
    test/FuzzableTerminalServer.hpp
    )
    add_dependencies(TerminalServerFuzzer TerminalCommon et-lib)
    target_link_libraries(
      TerminalServerFuzzer
      TerminalCommon
      et-lib
      ${CMAKE_THREAD_LIBS_INIT}
      ${PROTOBUF_LIBS}
      ${sodium_LIBRARY_RELEASE}
      ${SELINUX_LIBRARIES}
      ${UTEMPTER_LIBRARIES}
      ${Boost_LIBRARIES}
        ${CORE_LIBRARIES})
    decorate_fuzzer(TerminalServerFuzzer)

    add_executable(
    TerminalServerRouterFuzzer
    test/TerminalServerRouterFuzzer.cpp
    test/FuzzableTerminalServer.hpp
    )
    add_dependencies(TerminalServerRouterFuzzer TerminalCommon et-lib)
    target_link_libraries(
      TerminalServerRouterFuzzer
      TerminalCommon
      et-lib
      ${CMAKE_THREAD_LIBS_INIT}
      ${PROTOBUF_LIBS}
      ${sodium_LIBRARY_RELEASE}
      ${SELINUX_LIBRARIES}
      ${UTEMPTER_LIBRARIES}
      ${Boost_LIBRARIES}
        ${CORE_LIBRARIES})
    decorate_fuzzer(TerminalServerRouterFuzzer)
  endif(FUZZING)

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

set(CPACK_PACKAGE_NAME "EternalTerminal")
set(CPACK_PACKAGE_VENDOR "https://github.com/MisterTea/EternalTerminal")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Remote terminal for the busy and impatient")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "EternalTerminal")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

SET(CPACK_GENERATOR "STGZ;TGZ;TZ;DEB")
SET(CPACK_DEBIAN_PACKAGE_MAINTAINER "Unmaintained")

# This must always be last!
include(CPack)
