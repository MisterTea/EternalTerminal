cmake_minimum_required(VERSION 3.10)
project(sentry_screenshot LANGUAGES C)

if(WIN32 AND NOT XBOX)
	add_executable(sentry_screenshot screenshot_win32.c)
	set_target_properties(sentry_screenshot PROPERTIES WIN32_EXECUTABLE TRUE)
	target_compile_definitions(sentry_screenshot PRIVATE _UNICODE UNICODE)
	target_link_libraries(sentry_screenshot PRIVATE sentry)

	if(MSVC)
		target_compile_options(sentry_screenshot PRIVATE $<BUILD_INTERFACE:/wd5105 /wd4717>)
		if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang") # clang-cl
			target_compile_options(sentry_screenshot PRIVATE $<BUILD_INTERFACE:-Wno-infinite-recursion>)
		endif()

		# to test handling SEH by-passing exceptions we need to enable the control flow guard
		target_compile_options(sentry_screenshot PRIVATE $<BUILD_INTERFACE:/guard:cf>)
	else()
		# Disable all optimizations for the `sentry_screenshot` in gcc/clang. This allows us to keep crash triggers simple.
		# The effects besides reproducible code-gen across compiler versions, will be negligible for build- and runtime.
		target_compile_options(sentry_screenshot PRIVATE $<BUILD_INTERFACE:-O0>)
		set_target_properties(sentry_screenshot PROPERTIES LINK_FLAGS -municode)
	endif()
endif()
