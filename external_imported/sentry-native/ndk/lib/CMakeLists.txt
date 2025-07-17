cmake_minimum_required(VERSION 3.10)
project(Sentry-Android LANGUAGES C CXX)

# Add sentry-android shared library
add_library(sentry-android SHARED src/main/jni/sentry.c)

# Conditionally add test library if ENABLE_TESTS is set
if(ENABLE_TESTS)
	add_library(sentry-android-test SHARED src/androidTest/jni/ndk-test.cpp)
endif()

# make sure that we build it as a shared lib instead of a static lib
set(BUILD_SHARED_LIBS ON)
set(SENTRY_BUILD_SHARED_LIBS ON)

# Adding sentry-native project
add_subdirectory(${SENTRY_NATIVE_SRC} sentry_build)

# Android logging library
find_library(LOG_LIB log)

if(ENABLE_TESTS)
	target_link_libraries(sentry-android-test PRIVATE
		${LOG_LIB}
		$<BUILD_INTERFACE:sentry::sentry>
	)
endif()

# Link to sentry-native
target_link_libraries(sentry-android PRIVATE $<BUILD_INTERFACE:sentry::sentry>)

# Support 16KB page sizes
target_link_options(sentry-android PRIVATE "-Wl,-z,max-page-size=16384")
