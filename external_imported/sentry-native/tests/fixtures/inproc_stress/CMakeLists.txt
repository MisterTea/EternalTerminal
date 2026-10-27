cmake_minimum_required(VERSION 3.10)
project(inproc_stress_test C)

# SENTRY_LIB_DIR and SENTRY_INCLUDE_DIR are passed from the pytest

# Options that match the main sentry-native build
option(SENTRY_BUILD_FORCE32 "Force a 32bit compile on a 64bit host" OFF)
option(WITH_ASAN_OPTION "Build with address sanitizer" OFF)
option(WITH_TSAN_OPTION "Build with thread sanitizer" OFF)

# Use CMake's thread finding mechanism (handles Android correctly)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(inproc_stress_test
    main.c
    concurrent_crash.c
)

target_include_directories(inproc_stress_test PRIVATE ${SENTRY_INCLUDE_DIR})
target_link_directories(inproc_stress_test PRIVATE ${SENTRY_LIB_DIR})
target_link_libraries(inproc_stress_test PRIVATE sentry Threads::Threads)

set_target_properties(inproc_stress_test PROPERTIES
    BUILD_RPATH "${SENTRY_LIB_DIR}"
    # Ensure the executable is placed directly in the build directory on all platforms
    # (Visual Studio generators normally use Debug/Release subdirectories)
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}"
)

if(NOT MSVC)
    target_compile_options(inproc_stress_test PRIVATE -O0 -g)

    if(SENTRY_BUILD_FORCE32)
        target_compile_options(inproc_stress_test PRIVATE -m32)
        target_link_options(inproc_stress_test PRIVATE -m32)
    endif()

    if(WITH_ASAN_OPTION)
        target_compile_options(inproc_stress_test PRIVATE -fsanitize=address)
        target_link_options(inproc_stress_test PRIVATE -fsanitize=address)
    endif()

    if(WITH_TSAN_OPTION)
        target_compile_options(inproc_stress_test PRIVATE -fsanitize=thread)
        target_link_options(inproc_stress_test PRIVATE -fsanitize=thread)
    endif()
endif()
