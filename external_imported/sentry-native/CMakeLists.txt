if(WIN32)
	cmake_minimum_required(VERSION 3.18)

	# enables support for CMAKE_MSVC_RUNTIME_LIBRARY
	cmake_policy(SET CMP0091 NEW)
else()
	# The Android tools ship with this ancient version, which we need to support.
	cmake_minimum_required(VERSION 3.10)
	cmake_policy(SET CMP0077 NEW)
endif()

# Allow target_link_libraries() in subdirectories
if(POLICY CMP0079)
	cmake_policy(SET CMP0079 NEW)
endif()

# Allow downstream SDKs to override the SDK version at CMake configuration time
set(SENTRY_SDK_VERSION "" CACHE STRING "Override the SDK version (supports full semver format with build metadata)")

# Extract version string from SENTRY_SDK_VERSION cache variable or sentry.h
if(NOT SENTRY_SDK_VERSION STREQUAL "")
	set(SENTRY_VERSION_FULL "${SENTRY_SDK_VERSION}")
else()
	file(READ "include/sentry.h" _VERSION_STR_TMP)
	# Supports full semver format including prerelease and build metadata
	# The regex handles optional leading whitespace for the #define
	string(REGEX MATCH "#[ \t]*define[ \t]+SENTRY_SDK_VERSION[ \t]+\"([^\"]+)\"" _VERSION_STR_TMP "${_VERSION_STR_TMP}")
	set(SENTRY_VERSION_FULL "${CMAKE_MATCH_1}")
	unset(_VERSION_STR_TMP)
endif()

# Extract just the major.minor.patch part from the full version
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _VERSION_PARSE_TMP "${SENTRY_VERSION_FULL}")
set(SENTRY_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(SENTRY_VERSION_MINOR "${CMAKE_MATCH_2}")
set(SENTRY_VERSION_PATCH "${CMAKE_MATCH_3}")
set(SENTRY_VERSION_BASE "${SENTRY_VERSION_MAJOR}.${SENTRY_VERSION_MINOR}.${SENTRY_VERSION_PATCH}")
unset(_VERSION_PARSE_TMP)

if(NOT SENTRY_SDK_VERSION STREQUAL "")
	message(STATUS "Sentry SDK version (full): ${SENTRY_VERSION_FULL} (overridden via SENTRY_SDK_VERSION)")
else()
	message(STATUS "Sentry SDK version (full): ${SENTRY_VERSION_FULL}")
endif()
message(STATUS "Sentry SDK version (base): ${SENTRY_VERSION_BASE}")
message(STATUS "Sentry SDK version major='${SENTRY_VERSION_MAJOR}' minor='${SENTRY_VERSION_MINOR}' patch='${SENTRY_VERSION_PATCH}'")

if(NOT SENTRY_VERSION_FULL OR NOT SENTRY_VERSION_BASE OR SENTRY_VERSION_MAJOR STREQUAL "" OR SENTRY_VERSION_MINOR STREQUAL "" OR SENTRY_VERSION_PATCH STREQUAL "")
    message(FATAL_ERROR "Parsed Sentry SDK version '${SENTRY_VERSION_FULL}' does not match expected semver format")
endif()

project(Sentry-Native
	LANGUAGES C CXX
	VERSION ${SENTRY_VERSION_BASE}
)

set(SENTRY_MAIN_PROJECT OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
	set(SENTRY_MAIN_PROJECT ON)
endif()

# We have 4 source directories:
# * `CMAKE_SOURCE_DIR`: points to the directory of the top-level CMakeLists.txt of the main project.
# * `CMAKE_CURRENT_SOURCE_DIR`: points to the directory of any CMakeLists.txt in any subdirectories.
# * `PROJECT_SOURCE_DIR`: points to the directory of any CMakeLists.txt in any subdirectories that defined a `project`.
# * `SENTRY_SOURCE_DIR`: points to the directory of this `CMakeLists.txt` independent of whether it was added as a
#                        subdirectory in another project or whether we access it from one of our subdirectories.
set(SENTRY_SOURCE_DIR ${PROJECT_SOURCE_DIR})

if(NOT CMAKE_C_STANDARD)
	set(CMAKE_C_STANDARD 99)
endif()

if(NOT CMAKE_CXX_STANDARD)
	set(CMAKE_CXX_STANDARD 17)
endif()

include(GNUInstallDirs)
set(CMAKE_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/sentry")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	set(LINUX TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "AIX" OR CMAKE_SYSTEM_NAME STREQUAL "OS400")
	set(AIX TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Prospero")
	set(PROSPERO TRUE)
elseif("${CMAKE_GENERATOR_PLATFORM}" MATCHES "Gaming\\.Xbox\\.(Scarlett|XboxOne)\\.x64")
	set(XBOX TRUE)
elseif("${CMAKE_GENERATOR_PLATFORM}" MATCHES "Gaming\\.Desktop\\.x64")
	set(WINGDK TRUE)
endif()

#setup sentry library type
if(SENTRY_MAIN_PROJECT AND NOT DEFINED BUILD_SHARED_LIBS)
	set(BUILD_SHARED_LIBS ON)
endif()
option(SENTRY_BUILD_SHARED_LIBS "Build shared libraries (.dll/.so) instead of static ones (.lib/.a)" ${BUILD_SHARED_LIBS})
if(SENTRY_BUILD_SHARED_LIBS)
	set(SENTRY_LIBRARY_TYPE SHARED)
else()
	set(SENTRY_LIBRARY_TYPE STATIC)
endif()

option(SENTRY_PIC "Build sentry (and dependent) libraries as position independent libraries" ON)

if(LINUX)
	option(SENTRY_LIBUNWIND_SYSTEM "Use system libunwind library" OFF)
else()
	set(SENTRY_LIBUNWIND_SYSTEM OFF)
endif()

option(SENTRY_TRANSPORT_COMPRESSION "Enable transport gzip compression" OFF)

option(SENTRY_BUILD_TESTS "Build sentry-native tests" "${SENTRY_MAIN_PROJECT}")
option(SENTRY_BUILD_EXAMPLES "Build sentry-native example(s)" "${SENTRY_MAIN_PROJECT}")
option(SENTRY_BUILD_BENCHMARKS "Build sentry-native benchmarks" OFF)

# Platform version embedding options
option(SENTRY_EMBED_INFO "Embed version information in binary" OFF)
set(SENTRY_BUILD_PLATFORM "${CMAKE_SYSTEM_NAME}" CACHE STRING "Platform name for embedded version (e.g., switch, playstation, xbox)")
set(SENTRY_BUILD_VARIANT "" CACHE STRING "Build variant for embedded version (e.g., native, unreal)")
set(SENTRY_BUILD_ID "" CACHE STRING "Build identifier for embedded version (defaults to timestamp)")
set(SENTRY_EMBED_INFO_ITEMS "" CACHE STRING "Additional items to embed (semicolon-separated key:value pairs)")

# Validate the format of custom items
if(SENTRY_EMBED_INFO_ITEMS)
	# Convert semicolon-separated string to list for iteration
	string(REPLACE ";" "\n" TEMP_ITEMS "${SENTRY_EMBED_INFO_ITEMS}")
	string(REPLACE "\n" ";" ITEMS_LIST "${TEMP_ITEMS}")
	foreach(ITEM IN LISTS ITEMS_LIST)
		if(ITEM AND NOT ITEM MATCHES "^[^:]+:[^:]*$")
			message(FATAL_ERROR "Invalid embedded info item format: ${ITEM}. Expected format: key:value")
		endif()
	endforeach()
endif()

if(NOT XBOX AND NOT WINGDK)
	option(SENTRY_LINK_PTHREAD "Link platform threads library" ON)
	if(SENTRY_LINK_PTHREAD)
		set(THREADS_PREFER_PTHREAD_FLAG ON)
	    find_package(Threads REQUIRED)
	endif()
endif()

if(MSVC)
	option(SENTRY_BUILD_RUNTIMESTATIC "Build sentry-native with static runtime" OFF)

	set(CMAKE_C_FLAGS    "${CMAKE_C_FLAGS} /utf-8")
	set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} /utf-8")
endif()

if(LINUX)
	option(SENTRY_BUILD_FORCE32 "Force a 32bit compile on a 64bit host" OFF)
	if(SENTRY_BUILD_FORCE32)
		set(CMAKE_C_FLAGS    "${CMAKE_C_FLAGS} -m32 -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE")
		set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -m32 -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE")
		set(CMAKE_ASM_FLAGS  "${CMAKE_ASM_FLAGS} -m32 -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE")
		set_property(GLOBAL PROPERTY FIND_LIBRARY_USE_LIB64_PATHS OFF)
		set(CMAKE_SYSTEM_PROCESSOR "i686")
	endif()
endif()

# CMAKE_POSITION_INDEPENDENT_CODE must be set BEFORE adding any libraries (including subprojects)
if(SENTRY_PIC)
	set(CMAKE_POSITION_INDEPENDENT_CODE ON)
else()
	set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
endif()

if(WIN32)
	set(SENTRY_DEFAULT_TRANSPORT "winhttp")
	set(_SUPPORTED_TRANSPORTS "none, winhttp, custom")
elseif(PROSPERO)
	set(SENTRY_DEFAULT_TRANSPORT "pshttp")
	set(_SUPPORTED_TRANSPORTS "none, pshttp")
elseif((APPLE AND NOT IOS) OR LINUX OR AIX OR NX)
	set(SENTRY_DEFAULT_TRANSPORT "curl")
	set(_SUPPORTED_TRANSPORTS "none, curl, custom")
else()
	set(SENTRY_DEFAULT_TRANSPORT "none")
	set(_SUPPORTED_TRANSPORTS "none, curl, winhttp on Windows, or custom")
endif()

set(SENTRY_TRANSPORT ${SENTRY_DEFAULT_TRANSPORT} CACHE STRING
	"The HTTP transport that sentry uses to submit events to the sentry server, can be one of: ${_SUPPORTED_TRANSPORTS}.")

if(SENTRY_TRANSPORT STREQUAL "winhttp")
	set(SENTRY_TRANSPORT_WINHTTP TRUE)
	if(NOT WIN32)
		message(FATAL_ERROR "The winhttp transport is only supported on Windows.")
	endif()
elseif(SENTRY_TRANSPORT STREQUAL "curl")
	set(SENTRY_TRANSPORT_CURL TRUE)
elseif(SENTRY_TRANSPORT STREQUAL "none")
	set(SENTRY_TRANSPORT_NONE TRUE)
elseif(SENTRY_TRANSPORT STREQUAL "pshttp")
	# Not implemented here, but in the downstream SDK
	if(NOT PROSPERO)
		message(FATAL_ERROR "The pshttp transport is only supported on PlayStation.")
	endif()
elseif(SENTRY_TRANSPORT STREQUAL "custom")
	set(SENTRY_TRANSPORT_CUSTOM TRUE)
else()
	message(FATAL_ERROR "SENTRY_TRANSPORT must be one of: ${_SUPPORTED_TRANSPORTS}")
endif()

if(WIN32 AND NOT XBOX)
	set(SENTRY_DEFAULT_SCREENSHOT "windows")
	set(_SUPPORTED_SCREENSHOTS "windows, none, or custom")
else()
	set(SENTRY_DEFAULT_SCREENSHOT "none")
	set(_SUPPORTED_SCREENSHOTS "none or custom")
endif()

set(SENTRY_SCREENSHOT ${SENTRY_DEFAULT_SCREENSHOT} CACHE STRING
	"The screenshot implementation that sentry uses, can be one of: ${_SUPPORTED_SCREENSHOTS}.")

if(SENTRY_SCREENSHOT STREQUAL "windows")
	if(NOT WIN32 OR XBOX)
		message(FATAL_ERROR "The windows screenshot implementation is only supported on Windows.")
	endif()
	set(SENTRY_SCREENSHOT_WINDOWS TRUE)
elseif(SENTRY_SCREENSHOT STREQUAL "custom")
	set(SENTRY_SCREENSHOT_CUSTOM TRUE)
elseif(SENTRY_SCREENSHOT STREQUAL "none")
	set(SENTRY_SCREENSHOT_NONE TRUE)
else()
	message(FATAL_ERROR "SENTRY_SCREENSHOT must be one of: ${_SUPPORTED_SCREENSHOTS}")
endif()

if(SENTRY_BUILD_TESTS OR SENTRY_BUILD_EXAMPLES)
	enable_testing()
endif()

if("${CMAKE_SOURCE_DIR}" STREQUAL "${PROJECT_SOURCE_DIR}")
	set(SENTRY_MAIN_PROJECT ON)
endif()

option(SENTRY_ENABLE_INSTALL "Enable sentry installation" "${SENTRY_MAIN_PROJECT}")

if(MSVC AND CMAKE_GENERATOR_TOOLSET MATCHES "_xp$")
	message(WARNING "Crashpad is not supported for MSVC with XP toolset. Default backend was switched to 'breakpad'")
	set(SENTRY_DEFAULT_BACKEND "breakpad")
elseif(MSVC AND (XBOX OR WINGDK))
	set(SENTRY_DEFAULT_BACKEND "breakpad")
elseif((APPLE AND NOT IOS) OR WIN32 OR LINUX)
	set(SENTRY_DEFAULT_BACKEND "crashpad")
else()
	set(SENTRY_DEFAULT_BACKEND "inproc")
endif()

# Native backend is available on all platforms as an alternative
# It's lightweight (~5K LOC) and supports all platforms

if(NOT DEFINED SENTRY_BACKEND)
	set(SENTRY_BACKEND ${SENTRY_DEFAULT_BACKEND} CACHE STRING
		"The sentry backend responsible for reporting crashes, can be either 'none', 'inproc', 'breakpad', 'crashpad', or 'native'.")
endif()

if(SENTRY_BACKEND STREQUAL "crashpad")
	set(SENTRY_BACKEND_CRASHPAD TRUE)
elseif(SENTRY_BACKEND STREQUAL "inproc")
	set(SENTRY_BACKEND_INPROC TRUE)
elseif(SENTRY_BACKEND STREQUAL "breakpad")
	set(SENTRY_BACKEND_BREAKPAD TRUE)
elseif(SENTRY_BACKEND STREQUAL "native")
	message(WARNING "The 'native' backend is experimental and under active development. "
		"It may have incomplete features or unexpected behavior.")
	set(SENTRY_BACKEND_NATIVE TRUE)
elseif(SENTRY_BACKEND STREQUAL "none")
	set(SENTRY_BACKEND_NONE TRUE)
elseif(SENTRY_BACKEND STREQUAL "custom")
	message(DEBUG
		"SENTRY_BACKEND set to 'custom' - a custom backend source must be added to the compilation unit by the downstream SDK.")
else()
	message(FATAL_ERROR
		"SENTRY_BACKEND must be one of 'crashpad', 'inproc', 'breakpad', 'native', or 'none'.
		Downstream SDKs may choose to provide their own by specifying 'custom'.")
endif()

if(SENTRY_BACKEND_CRASHPAD AND ANDROID)
	message(FATAL_ERROR "The Crashpad backend is not currently supported on Android")
endif()

set(SENTRY_SDK_NAME "" CACHE STRING "The SDK name to report when sending events.")
set(SENTRY_HANDLER_STACK_SIZE 64 CACHE STRING "The stack size (in KiB) that should be reserved for the crash handler.")
if (WIN32)
	set(SENTRY_THREAD_STACK_GUARANTEE_FACTOR 10 CACHE STRING "The factor by which a threads stack should be larger than the stack guarantee for its handler.")
	option(SENTRY_THREAD_STACK_GUARANTEE_AUTO_INIT "Automatically sets the thread stack guarantee for each thread via `DllMain` or for the `sentry_init()` when building statically" ON)
	option(SENTRY_THREAD_STACK_GUARANTEE_VERBOSE_LOG "Enables logging of successfully set thread stack guarantees" OFF)
endif()

message(STATUS "SENTRY_TRANSPORT=${SENTRY_TRANSPORT}")
message(STATUS "SENTRY_BACKEND=${SENTRY_BACKEND}")
message(STATUS "SENTRY_LIBRARY_TYPE=${SENTRY_LIBRARY_TYPE}")
message(STATUS "SENTRY_SDK_NAME=${SENTRY_SDK_NAME}")
message(STATUS "SENTRY_HANDLER_STACK_SIZE=${SENTRY_HANDLER_STACK_SIZE}")
if (WIN32)
	message(STATUS "SENTRY_THREAD_STACK_GUARANTEE_FACTOR=${SENTRY_THREAD_STACK_GUARANTEE_FACTOR}")
	message(STATUS "SENTRY_THREAD_STACK_GUARANTEE_AUTO_INIT=${SENTRY_THREAD_STACK_GUARANTEE_AUTO_INIT}")
	message(STATUS "SENTRY_THREAD_STACK_GUARANTEE_VERBOSE_LOG=${SENTRY_THREAD_STACK_GUARANTEE_VERBOSE_LOG}")
endif()

if(ANDROID)
	set(SENTRY_WITH_LIBUNWINDSTACK TRUE)
elseif(LINUX)
	set(SENTRY_WITH_LIBUNWIND TRUE)
elseif(APPLE)
	set(SENTRY_WITH_LIBUNWIND_MAC TRUE)
elseif(NOT WIN32 AND NOT PROSPERO)
	# this should never be true, but we keep it as fallback
	set(SENTRY_WITH_LIBBACKTRACE TRUE)
endif()

option(WITH_ASAN_OPTION "Build sentry-native with address sanitizer" OFF)
if(WITH_ASAN_OPTION)
	add_compile_options(-g -fsanitize=address -fno-omit-frame-pointer)
	link_libraries(-fsanitize=address)
endif()

option(WITH_TSAN_OPTION "Build sentry-native with thread sanitizer" OFF)
if(WITH_TSAN_OPTION)
	add_compile_options(-g -fsanitize=thread -fno-omit-frame-pointer)
	link_libraries(-fsanitize=thread)
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

# use -O3 when doing `RelWithDebInfo` builds
if(NOT MSVC)
	foreach(lang ASM C CXX)
		string(REPLACE "-O2" "-O3" CMAKE_${lang}_FLAGS_RELWITHDEBINFO "${CMAKE_${lang}_FLAGS_RELWITHDEBINFO}")
	endforeach()
endif()

# https://gitlab.kitware.com/cmake/cmake/issues/20256
if(APPLE)
	find_program(DSYMUTIL_PROGRAM dsymutil)
	if(DSYMUTIL_PROGRAM)
		foreach(lang C CXX)
			foreach(var LINK_EXECUTABLE CREATE_SHARED_LIBRARY)
				set(CMAKE_${lang}_${var} "${CMAKE_${lang}_${var}}" "${DSYMUTIL_PROGRAM} <TARGET>")
			endforeach()
		endforeach()
	endif()
endif()

function(sentry_install)
	if(SENTRY_ENABLE_INSTALL)
		install(${ARGN})
	endif()
endfunction()

# helper function to add sources to existing TARGET prepended with ${CMAKE_CURRENT_SOURCE_DIR}/${SUBDIR}
function(sentry_target_sources_cwd TARGET)
	cmake_parse_arguments(STSC "" "SUBDIR" "" ${ARGN})
	foreach(src ${STSC_UNPARSED_ARGUMENTS})
		if(IS_ABSOLUTE "${src}")
			target_sources(${TARGET} PRIVATE ${src})
		else()
			target_sources(${TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${STSC_SUBDIR}/${src}")
		endif()
	endforeach()
endfunction()

# ===== sentry library =====

add_library(sentry ${SENTRY_LIBRARY_TYPE} "${PROJECT_SOURCE_DIR}/vendor/mpack.c")
if (XBOX)
	set_target_properties(sentry PROPERTIES VS_USER_PROPS gdk_build.props)
endif()
target_sources(sentry PRIVATE "${PROJECT_SOURCE_DIR}/include/sentry.h")
add_library(sentry::sentry ALIAS sentry)

# Configure version embedding if enabled
if(SENTRY_EMBED_INFO)
	# SENTRY_BUILD_ID cache variable takes precedence
	if(SENTRY_BUILD_ID)
		set(SENTRY_EMBEDDED_BUILD_ID "${SENTRY_BUILD_ID}")
	elseif(SENTRY_VERSION_FULL MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\+(.+)$")
		# Extract build ID from SENTRY_VERSION_FULL if present (format: major.minor.patch+build-id)
		# This handles cases where downstream SDKs modify sentry.h to include build metadata
		set(SENTRY_EMBEDDED_BUILD_ID "${CMAKE_MATCH_1}")
	else()
		# No build ID found in version or provided, use timestamp as default
		# Format: YYYYMMDD-HHMMSS (semver-compliant: alphanumerics and hyphens only)
		string(TIMESTAMP SENTRY_EMBEDDED_BUILD_ID "%Y%m%d-%H%M%S" UTC)
	endif()

	# Validate and escape special characters in custom items if needed
	if(SENTRY_EMBED_INFO_ITEMS)
		# Sanitize input to prevent CMake substitution issues
		string(REPLACE "\"" "\\\"" SENTRY_EMBED_INFO_ITEMS "${SENTRY_EMBED_INFO_ITEMS}")
	endif()

	# Ensure custom items end with semicolon if not empty
	if(SENTRY_EMBED_INFO_ITEMS AND NOT SENTRY_EMBED_INFO_ITEMS MATCHES ";$")
		set(SENTRY_EMBED_INFO_ITEMS "${SENTRY_EMBED_INFO_ITEMS};")
	endif()

	# Ensure the output directory exists
	file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/src")

	configure_file(
		"${CMAKE_CURRENT_SOURCE_DIR}/src/sentry_embedded_info.cpp.in"
		"${CMAKE_CURRENT_BINARY_DIR}/src/sentry_embedded_info.cpp"
		@ONLY
	)

	target_sources(sentry PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/src/sentry_embedded_info.cpp")
endif()

add_subdirectory(src)

target_compile_definitions(sentry PRIVATE SENTRY_HANDLER_STACK_SIZE=${SENTRY_HANDLER_STACK_SIZE})
if(WIN32)
	target_compile_definitions(sentry PRIVATE SENTRY_THREAD_STACK_GUARANTEE_FACTOR=${SENTRY_THREAD_STACK_GUARANTEE_FACTOR})
	if (SENTRY_THREAD_STACK_GUARANTEE_AUTO_INIT)
		target_compile_definitions(sentry PRIVATE SENTRY_THREAD_STACK_GUARANTEE_AUTO_INIT)
	endif()
	if (SENTRY_THREAD_STACK_GUARANTEE_VERBOSE_LOG)
		target_compile_definitions(sentry PRIVATE SENTRY_THREAD_STACK_GUARANTEE_VERBOSE_LOG)
	endif()
endif()

if (NOT SENTRY_SDK_NAME STREQUAL "")
	target_compile_definitions(sentry PRIVATE SENTRY_SDK_NAME="${SENTRY_SDK_NAME}")
endif()

# If SDK version was overridden via CMake, pass it as a compile definition
# This allows sentry.h to use the overridden version
if (NOT SENTRY_SDK_VERSION STREQUAL "")
	target_compile_definitions(sentry PRIVATE SENTRY_SDK_VERSION="${SENTRY_SDK_VERSION}")
endif()

# we do not need this on android, only linux
if(LINUX)
	target_sources(sentry PRIVATE
		"${PROJECT_SOURCE_DIR}/vendor/stb_sprintf.c"
		"${PROJECT_SOURCE_DIR}/vendor/stb_sprintf.h"
	)
endif()

set_target_properties(sentry PROPERTIES PUBLIC_HEADER "include/sentry.h")

if(DEFINED SENTRY_FOLDER)
	set_target_properties(sentry PROPERTIES FOLDER ${SENTRY_FOLDER})
endif()

# check size type
include(CheckTypeSize)
check_type_size("long" CMAKE_SIZEOF_LONG)

if(LINUX)
	include(CheckSymbolExists)
	list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
	check_symbol_exists(copy_file_range "unistd.h" HAVE_COPY_FILE_RANGE)
	if(HAVE_COPY_FILE_RANGE)
		target_compile_definitions(sentry PRIVATE SENTRY_HAVE_COPY_FILE_RANGE)
	endif()
endif()

# https://gitlab.kitware.com/cmake/cmake/issues/18393
if(SENTRY_BUILD_SHARED_LIBS)
	if(APPLE)
		sentry_install(FILES "$<TARGET_FILE:sentry>.dSYM" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
	elseif(MSVC)
		sentry_install(FILES "$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:$<TARGET_PDB_FILE:sentry>>"
			DESTINATION "${CMAKE_INSTALL_BINDIR}")
	endif()
endif()

if(SENTRY_BUILD_SHARED_LIBS)
	target_compile_definitions(sentry PRIVATE SENTRY_BUILD_SHARED)
else()
	target_compile_definitions(sentry PUBLIC SENTRY_BUILD_STATIC)
endif()
target_compile_definitions(sentry PRIVATE SIZEOF_LONG=${CMAKE_SIZEOF_LONG})

# AIX needs libm for isnan used in test suite
if(CMAKE_SYSTEM_NAME STREQUAL "AIX" OR CMAKE_SYSTEM_NAME STREQUAL "OS400")
	target_link_libraries(sentry PRIVATE m)
endif()
# On IBM i PASE, flock is in libutil. Here because "sentry" exists now.
if(CMAKE_SYSTEM_NAME STREQUAL "OS400")
	target_link_libraries(sentry PRIVATE util)
endif()

if(SENTRY_TRANSPORT_CURL)
	if(NOT TARGET CURL::libcurl) # Some other lib might bring libcurl already
		find_package(CURL REQUIRED)
	endif()

	target_link_libraries(sentry PRIVATE CURL::libcurl)
endif()

if(SENTRY_TRANSPORT_COMPRESSION)
	if(NOT TARGET ZLIB::ZLIB)
		find_package(ZLIB REQUIRED)
	endif()

	if(SENTRY_BACKEND_CRASHPAD)
		set(CRASHPAD_ZLIB_SYSTEM ON CACHE BOOL "Force CRASHPAD_ZLIB_SYSTEM when enabling transport compression" FORCE)
	endif()

	target_link_libraries(sentry PRIVATE ZLIB::ZLIB)
	target_compile_definitions(sentry PRIVATE SENTRY_TRANSPORT_COMPRESSION)
endif()

set_property(TARGET sentry PROPERTY C_VISIBILITY_PRESET hidden)
if(MSVC)
	if(CMAKE_SIZEOF_VOID_P EQUAL 4)
		set(CMAKE_ASM_MASM_FLAGS "${CMAKE_ASM_MASM_FLAGS} /safeseh")
	endif()

	# using `/Wall` is not feasible, as it spews tons of warnings from windows headers
	# supress C5105, introduced in VS 16.8, which breaks on the Windows SDKs own `winbase.h` header
	if (XBOX)
		target_compile_options(sentry PRIVATE $<BUILD_INTERFACE:/W4 /wd5105 /wd4115>)
	else()
		target_compile_options(sentry PRIVATE $<BUILD_INTERFACE:/W4 /wd5105>)
	endif()
	# ignore all warnings for mpack
	set_source_files_properties(
		"${PROJECT_SOURCE_DIR}/vendor/mpack.c"
		PROPERTIES
		COMPILE_FLAGS
		"/W0"
	)

	# set static runtime if enabled
	if(SENTRY_BUILD_RUNTIMESTATIC)
		set_property(TARGET sentry PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()

	if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang") # clang-cl
		target_compile_options(sentry PRIVATE
			$<BUILD_INTERFACE:-Werror -Wall>
			$<BUILD_INTERFACE:-Wno-unsafe-buffer-usage>
			$<BUILD_INTERFACE:-Wno-nonportable-system-include-path>
			$<BUILD_INTERFACE:-Wno-declaration-after-statement>
			$<BUILD_INTERFACE:-Wno-cast-qual>
			$<BUILD_INTERFACE:-Wno-switch-default>
			$<BUILD_INTERFACE:-Wno-cast-function-type>
			$<BUILD_INTERFACE:-Wno-cast-function-type-strict>
			$<BUILD_INTERFACE:-Wno-covered-switch-default>
			$<BUILD_INTERFACE:-Wno-format-nonliteral>
			$<BUILD_INTERFACE:-Wno-unused-macros>
			$<BUILD_INTERFACE:-Wno-reserved-macro-identifier>
			$<BUILD_INTERFACE:-Wno-assign-enum>
			$<BUILD_INTERFACE:-Wno-c++98-compat-pedantic>
			$<BUILD_INTERFACE:-Wno-reserved-identifier>
			$<BUILD_INTERFACE:-Wno-old-style-cast>
			$<BUILD_INTERFACE:-Wno-zero-as-null-pointer-constant>
		)
	endif()
else()
	target_compile_options(sentry PRIVATE $<BUILD_INTERFACE:-Wall -Wextra -Wpedantic>)
	# The crashpad and breakpad headers generate the following warnings that we
	# ignore specifically
	if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		target_compile_options(sentry PRIVATE $<BUILD_INTERFACE:-Wno-variadic-macros -Wno-multichar>)
	else()
		target_compile_options(sentry PRIVATE $<BUILD_INTERFACE:-Wno-variadic-macros -Wno-gnu-include-next -Wno-multichar>)
	endif()
	# ignore all warnings for mpack
	set_source_files_properties(
		"${PROJECT_SOURCE_DIR}/vendor/mpack.c"
		PROPERTIES
		COMPILE_FLAGS
		"-w"
	)
endif()


target_include_directories(sentry
	PUBLIC
		"$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
		"$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
	PRIVATE
		"$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
)


#respect CMAKE_SYSTEM_VERSION
if(WIN32)
	if(MSVC AND CMAKE_GENERATOR_TOOLSET MATCHES "_xp$")
		#force WINNT to 5.1 for Windows XP toolchain
		target_compile_definitions(sentry PRIVATE "_WIN32_WINNT=0x0501")
	elseif(${CMAKE_SYSTEM_VERSION} MATCHES "^10")
		target_compile_definitions(sentry PRIVATE "_WIN32_WINNT=0x0A00")
	elseif(${CMAKE_SYSTEM_VERSION} MATCHES "^6.3")
		target_compile_definitions(sentry PRIVATE "_WIN32_WINNT=0x0603")
	elseif(${CMAKE_SYSTEM_VERSION} MATCHES "^6.2")
		target_compile_definitions(sentry PRIVATE "_WIN32_WINNT=0x0602")
	elseif(${CMAKE_SYSTEM_VERSION} MATCHES "^6.1")
		target_compile_definitions(sentry PRIVATE "_WIN32_WINNT=0x0601")
	elseif(${CMAKE_SYSTEM_VERSION} MATCHES "^6.0")
		target_compile_definitions(sentry PRIVATE "_WIN32_WINNT=0x0600")
	elseif(${CMAKE_SYSTEM_VERSION} MATCHES "^5.2")
		target_compile_definitions(sentry PRIVATE "_WIN32_WINNT=0x0502")
	elseif(${CMAKE_SYSTEM_VERSION} MATCHES "^5.1")
		target_compile_definitions(sentry PRIVATE "_WIN32_WINNT=0x0501")
	endif()

	# crashpad does not support Windows XP toolset
	if(MSVC AND CMAKE_GENERATOR_TOOLSET MATCHES "_xp$" AND SENTRY_BACKEND_CRASHPAD)
		message(FATAL_ERROR "MSVC XP toolset does not support Crashpad")
	endif()
endif()

include(cmake/utils.cmake)
if (WIN32 AND SENTRY_BUILD_SHARED_LIBS)
	sentry_add_version_resource(sentry "Client Library")
endif()

# handle platform libraries
if(ANDROID)
	set(_SENTRY_PLATFORM_LIBS "dl" "log")
elseif(LINUX)
	set(_SENTRY_PLATFORM_LIBS "dl" "rt")
elseif(WIN32)
	if (XBOX)
		set(_SENTRY_PLATFORM_LIBS "version")
	else()
		set(_SENTRY_PLATFORM_LIBS "dbghelp" "shlwapi" "version")
	endif()
	# WaitOnAddress/WakeByAddressSingle require Windows 8+
	# (Synchronization.lib). Older targets fall back to sleep-poll.
	if(NOT (MSVC AND CMAKE_GENERATOR_TOOLSET MATCHES "_xp$")
		AND ("${CMAKE_SYSTEM_VERSION}" MATCHES "^10"
			OR "${CMAKE_SYSTEM_VERSION}" MATCHES "^6\\.[23]"))
		list(APPEND _SENTRY_PLATFORM_LIBS "synchronization")
	endif()
endif()

if(SENTRY_TRANSPORT_WINHTTP)
	list(APPEND _SENTRY_PLATFORM_LIBS "winhttp")
endif()

# handle platform threads library
if(NOT XBOX AND NOT WINGDK)
	if(SENTRY_LINK_PTHREAD)
		list(APPEND _SENTRY_PLATFORM_LIBS "Threads::Threads")
	endif()
endif()

# apply platform libraries to sentry library
target_link_libraries(sentry PRIVATE ${_SENTRY_PLATFORM_LIBS})

# suppress some errors and warnings for MinGW target
if(MINGW)
	target_compile_options(sentry PRIVATE
		-Wno-unused-variable
		-Wno-unused-parameter
		-Wno-format
		-Wno-incompatible-pointer-types
		-Wno-incompatible-function-pointer-types
	)
endif()

if(SENTRY_WITH_LIBUNWINDSTACK)
	target_include_directories(sentry PRIVATE
		"$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/external/libunwindstack-ndk/include>")
	add_subdirectory("${PROJECT_SOURCE_DIR}/external/libunwindstack-ndk/cmake")
	target_link_libraries(sentry PRIVATE unwindstack)
	if(NOT SENTRY_BUILD_SHARED_LIBS)
		sentry_install(TARGETS unwindstack EXPORT sentry
			LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
			ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
		)
	endif()
endif()

if(SENTRY_WITH_LIBUNWIND)
	if(LINUX)
		if(SENTRY_LIBUNWIND_SYSTEM)
			find_package(PkgConfig REQUIRED)
			pkg_check_modules(LIBUNWIND REQUIRED IMPORTED_TARGET libunwind)
			if(SENTRY_BUILD_SHARED_LIBS)
				target_link_libraries(sentry PRIVATE PkgConfig::LIBUNWIND)
			else()
				target_link_libraries(sentry PUBLIC PkgConfig::LIBUNWIND)
			endif()
		else()
			# Use vendored libunwind
			if(SENTRY_BACKEND_NATIVE)
				# Build remote unwinding support for the native backend's crash daemon
				set(LIBUNWIND_BUILD_REMOTE TRUE)
			endif()
			add_subdirectory(vendor/libunwind)
			target_link_libraries(sentry PRIVATE unwind)
			if(NOT SENTRY_BUILD_SHARED_LIBS)
				sentry_install(TARGETS unwind EXPORT sentry
					LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
					ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
				)
			endif()
		endif()
	endif()
endif()

if(SENTRY_BACKEND_CRASHPAD)
	if(SENTRY_BUILD_SHARED_LIBS)
		set(CRASHPAD_ENABLE_INSTALL OFF CACHE BOOL "Enable crashpad installation" FORCE)
	else()
		set(CRASHPAD_ENABLE_INSTALL ON CACHE BOOL "Enable crashpad installation" FORCE)
	endif()
	add_subdirectory(external/crashpad crashpad_build)

	if(WIN32)
		add_dependencies(sentry crashpad::wer)
	endif()

	# set static runtime if enabled
	if(SENTRY_BUILD_RUNTIMESTATIC AND MSVC)
		set_property(TARGET crashpad_client PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		set_property(TARGET crashpad_compat PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		set_property(TARGET crashpad_getopt PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		set_property(TARGET crashpad_handler PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		set_property(TARGET crashpad_handler_lib PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		set_property(TARGET crashpad_minidump PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		set_property(TARGET crashpad_mpack PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		set_property(TARGET crashpad_snapshot PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		set_property(TARGET crashpad_tools PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		set_property(TARGET crashpad_util PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		set_property(TARGET crashpad_wer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		set_property(TARGET crashpad_zlib PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		set_property(TARGET mini_chromium PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()

	if(DEFINED SENTRY_FOLDER)
		set_target_properties(crashpad_client PROPERTIES FOLDER ${SENTRY_FOLDER})
		set_target_properties(crashpad_compat PROPERTIES FOLDER ${SENTRY_FOLDER})
		set_target_properties(crashpad_getopt PROPERTIES FOLDER ${SENTRY_FOLDER})
		set_target_properties(crashpad_handler PROPERTIES FOLDER ${SENTRY_FOLDER})
		set_target_properties(crashpad_handler_lib PROPERTIES FOLDER ${SENTRY_FOLDER})
		set_target_properties(crashpad_minidump PROPERTIES FOLDER ${SENTRY_FOLDER})
		set_target_properties(crashpad_mpack PROPERTIES FOLDER ${SENTRY_FOLDER})
		set_target_properties(crashpad_snapshot PROPERTIES FOLDER ${SENTRY_FOLDER})
		set_target_properties(crashpad_tools PROPERTIES FOLDER ${SENTRY_FOLDER})
		set_target_properties(crashpad_util PROPERTIES FOLDER ${SENTRY_FOLDER})
		set_target_properties(crashpad_wer PROPERTIES FOLDER ${SENTRY_FOLDER})
		set_target_properties(crashpad_zlib PROPERTIES FOLDER ${SENTRY_FOLDER})
		set_target_properties(mini_chromium PROPERTIES FOLDER ${SENTRY_FOLDER})
	endif()

	target_link_libraries(sentry PRIVATE
		$<BUILD_INTERFACE:crashpad::client>
		$<INSTALL_INTERFACE:sentry_crashpad::client>
	)

	# ignore #include_next <signal.h> warning being a GCC extension via pedantic
	if (LINUX)
		target_include_directories(sentry
				SYSTEM PRIVATE
				external/crashpad/compat/linux
		)
	endif()

	install(EXPORT crashpad_export NAMESPACE sentry_crashpad:: FILE sentry_crashpad-targets.cmake
		DESTINATION "${CMAKE_INSTALL_CMAKEDIR}"
	)
	if(WIN32 AND MSVC)
		sentry_install(FILES $<TARGET_PDB_FILE:crashpad_handler>
			DESTINATION "${CMAKE_INSTALL_BINDIR}" OPTIONAL)
		sentry_install(FILES $<TARGET_PDB_FILE:crashpad_wer>
			DESTINATION "${CMAKE_INSTALL_BINDIR}" OPTIONAL)
	endif()
	add_dependencies(sentry crashpad::handler)
elseif(SENTRY_BACKEND_BREAKPAD)
	option(SENTRY_BREAKPAD_SYSTEM "Use system breakpad" OFF)
	if(SENTRY_BREAKPAD_SYSTEM)
		target_compile_definitions(sentry PRIVATE SENTRY_BREAKPAD_SYSTEM)
		# system breakpad is using pkg-config, see `external/breakpad/breakpad-client.pc.in`
		find_package(PkgConfig REQUIRED)
		pkg_check_modules(BREAKPAD REQUIRED IMPORTED_TARGET breakpad-client)
		if(SENTRY_BUILD_SHARED_LIBS)
			target_link_libraries(sentry PRIVATE PkgConfig::BREAKPAD)
		else()
			target_link_libraries(sentry PUBLIC PkgConfig::BREAKPAD)
		endif()
	else()
		add_subdirectory(external)
		target_include_directories(sentry PRIVATE
			"$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/external/breakpad/src>"
		)
		target_link_libraries(sentry PRIVATE
			breakpad_client
		)

		if(DEFINED SENTRY_FOLDER)
			set_target_properties(breakpad_client PROPERTIES FOLDER ${SENTRY_FOLDER})
		endif()

		if(NOT SENTRY_BUILD_SHARED_LIBS)
			sentry_install(TARGETS breakpad_client EXPORT sentry
				LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
				ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
			)
		endif()
	endif()
elseif(SENTRY_BACKEND_INPROC)
	target_compile_definitions(sentry PRIVATE SENTRY_WITH_INPROC_BACKEND)
elseif(SENTRY_BACKEND_NATIVE)
	target_compile_definitions(sentry PRIVATE SENTRY_WITH_NATIVE_BACKEND)

	# Native backend sources and configuration are in src/CMakeLists.txt
	# The native backend requires C11 for atomics (set in src/CMakeLists.txt)

	if(WIN32 AND NOT XBOX)
		add_library(sentry-wer SHARED
			src/backends/native/sentry_wer.c
			src/backends/native/sentry_wer.def
		)
		target_include_directories(sentry-wer PRIVATE
			${PROJECT_SOURCE_DIR}/include
			${PROJECT_SOURCE_DIR}/src
			${PROJECT_SOURCE_DIR}/src/backends/native
		)
		target_link_libraries(sentry-wer PRIVATE wer)
		set_property(TARGET sentry-wer PROPERTY PREFIX "") # ensure MINGW doesn't prefix "lib" to dll name
		set_property(TARGET sentry-wer PROPERTY DEBUG_POSTFIX "") # prevent CMAKE_DEBUG_POSTFIX from being applied
		if(SENTRY_BUILD_RUNTIMESTATIC AND MSVC)
			set_property(TARGET sentry-wer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		endif()
		sentry_add_version_resource(sentry-wer "Native WER Module")
	endif()

	# Build sentry-crash executable for native backend
	# Get all sources that were added to sentry target
	get_target_property(SENTRY_SOURCES sentry SOURCES)

	# Create daemon executable with same sources (already includes
	# sentry_crash_daemon.c and sentry_crash_ipc.c from src/CMakeLists.txt)
	add_executable(sentry-crash
		${SENTRY_SOURCES}
	)

	# Copy all compile definitions from sentry target so shared sources
	# compile under identical preprocessor conditions (backend, transport,
	# unwinder, compression, etc.)
	get_target_property(SENTRY_COMPILE_DEFS sentry COMPILE_DEFINITIONS)
	if(SENTRY_COMPILE_DEFS)
		# The daemon is always a static executable, never a shared library
		list(REMOVE_ITEM SENTRY_COMPILE_DEFS "SENTRY_BUILD_SHARED")
		target_compile_definitions(sentry-crash PRIVATE ${SENTRY_COMPILE_DEFS})
	endif()

	# Add daemon-specific definitions
	target_compile_definitions(sentry-crash PRIVATE
		SENTRY_CRASH_DAEMON_STANDALONE
		SENTRY_BUILD_STATIC
	)

	if(SENTRY_WITH_LIBUNWIND AND LINUX)
		target_compile_definitions(sentry-crash PRIVATE SENTRY_WITH_UNWINDER_LIBUNWIND_REMOTE)
	endif()

	# Copy include directories from sentry target
	target_include_directories(sentry-crash PRIVATE
		${PROJECT_SOURCE_DIR}/include
		${PROJECT_SOURCE_DIR}/src
		${PROJECT_SOURCE_DIR}/src/backends/native
	)

	# Link same libraries as sentry
	target_link_libraries(sentry-crash PRIVATE ${_SENTRY_PLATFORM_LIBS})
	if(APPLE)
		find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
		find_library(SECURITY_LIBRARY Security REQUIRED)
		target_link_libraries(sentry-crash PRIVATE
			${COREFOUNDATION_LIBRARY}
			${SECURITY_LIBRARY}
		)
	endif()

	# Transport-specific libraries
	if(SENTRY_TRANSPORT_CURL)
		target_link_libraries(sentry-crash PRIVATE CURL::libcurl)
	endif()

	if(SENTRY_TRANSPORT_WINHTTP)
		target_link_libraries(sentry-crash PRIVATE winhttp)
	endif()


	# Compression library
	if(SENTRY_TRANSPORT_COMPRESSION)
		target_link_libraries(sentry-crash PRIVATE ZLIB::ZLIB)
	endif()

	# Unwinder libraries (must match sentry target)
	if(SENTRY_WITH_LIBUNWINDSTACK)
		target_link_libraries(sentry-crash PRIVATE unwindstack)
	endif()

	if(SENTRY_WITH_LIBUNWIND AND LINUX)
		if(SENTRY_LIBUNWIND_SYSTEM)
			pkg_check_modules(LIBUNWIND_PTRACE REQUIRED IMPORTED_TARGET libunwind-ptrace)
			target_link_libraries(sentry-crash PRIVATE PkgConfig::LIBUNWIND PkgConfig::LIBUNWIND_PTRACE)
		else()
			# Use unwind_remote for the daemon (includes ptrace accessors
			# for remote DWARF unwinding of the crashed process)
			target_link_libraries(sentry-crash PRIVATE unwind_remote)
		endif()
	endif()

	# Make sentry library depend on crash daemon so it's always built together
	add_dependencies(sentry sentry-crash)
	if(WIN32 AND NOT XBOX)
		add_dependencies(sentry sentry-wer)
	endif()

	# Install daemon
	install(TARGETS sentry-crash
		RUNTIME DESTINATION bin
	)
	if(WIN32 AND NOT XBOX)
		install(TARGETS sentry-wer
			RUNTIME DESTINATION bin
			LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
			ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
		)
		if(MSVC)
			sentry_install(FILES $<TARGET_PDB_FILE:sentry-wer>
				DESTINATION "${CMAKE_INSTALL_BINDIR}" OPTIONAL)
		endif()
	endif()

	if(DEFINED SENTRY_FOLDER)
		# Native backend doesn't have separate targets to organize
	endif()

	message(STATUS "Sentry crash daemon executable: enabled")
endif()

option(SENTRY_INTEGRATION_QT "Build Qt integration")
if(SENTRY_INTEGRATION_QT)
	if(QT_DEFAULT_MAJOR_VERSION)
		# Let user choose major version
		set(Qt_VERSION_MAJOR ${QT_DEFAULT_MAJOR_VERSION})
	else()
		# Find best match, prioritizing Qt 6 if available
		find_package(Qt NAMES Qt6 Qt5 COMPONENTS Core REQUIRED)
	endif()
	find_package(Qt${Qt_VERSION_MAJOR} COMPONENTS Core REQUIRED)
	message(STATUS "Found Qt: ${Qt${Qt_VERSION_MAJOR}_DIR} "
		"(found version \"${Qt${Qt_VERSION_MAJOR}_VERSION}\")")
	target_link_libraries(sentry PRIVATE Qt${Qt_VERSION_MAJOR}::Core)
endif()

include(CMakePackageConfigHelpers)
configure_package_config_file(sentry-config.cmake.in sentry-config.cmake
	INSTALL_DESTINATION "${CMAKE_INSTALL_CMAKEDIR}")

# generate package version file
# We would have liked to use `SameMinorVersion`, but that is only supported on
# CMake >= 3.11.
write_basic_package_version_file(sentry-config-version.cmake
	VERSION ${SENTRY_VERSION_BASE}
	COMPATIBILITY SameMajorVersion)

sentry_install(TARGETS sentry EXPORT sentry
	ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
	LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
	RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
	PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
sentry_install(EXPORT sentry NAMESPACE sentry:: FILE sentry-targets.cmake
	DESTINATION "${CMAKE_INSTALL_CMAKEDIR}")
sentry_install(
	FILES
		"${PROJECT_BINARY_DIR}/sentry-config.cmake"
		"${PROJECT_BINARY_DIR}/sentry-config-version.cmake"
	DESTINATION "${CMAKE_INSTALL_CMAKEDIR}")
if(WIN32 AND MSVC AND SENTRY_BUILD_SHARED_LIBS)
	sentry_install(FILES $<TARGET_PDB_FILE:sentry>
		DESTINATION "${CMAKE_INSTALL_BINDIR}" OPTIONAL)
endif()

# ===== tests =====

if(SENTRY_BUILD_TESTS)
	add_subdirectory(tests/unit)
	add_subdirectory(tests/fixtures/crash_reporter)
	add_subdirectory(tests/fixtures/screenshot)
	if(SENTRY_BUILD_BENCHMARKS)
		set(BENCHMARK_ENABLE_TESTING OFF)
		set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
		add_subdirectory(external/benchmark)
		add_subdirectory(tests/benchmark)
	endif()
endif()

# ===== example, also used as integration test =====

if(SENTRY_BUILD_EXAMPLES)
	add_executable(sentry_example examples/example.c)
	if(XBOX)
		set_target_properties(sentry_example PROPERTIES VS_USER_PROPS gdk_build.props)
	endif()
	target_link_libraries(sentry_example PRIVATE sentry)

	if(MSVC)
		target_compile_options(sentry_example PRIVATE $<BUILD_INTERFACE:/wd5105 /wd4717>)
		if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang") # clang-cl
			# ignore the warning for the intentionally infinite recursion
			target_compile_options(sentry_example PRIVATE $<BUILD_INTERFACE:-Wno-infinite-recursion>)
			# ensure that clang-cl builds an unoptimized `sentry_example` in all build types
			target_compile_options(sentry_example PRIVATE $<BUILD_INTERFACE:/Od>)
		endif()

		# to test handling SEH by-passing exceptions we need to enable the control flow guard
		target_compile_options(sentry_example PRIVATE $<BUILD_INTERFACE:/guard:cf>)
		target_link_options(sentry_example PRIVATE $<BUILD_INTERFACE:/GUARD:CF>)
	else()
		# Disable all optimizations for the `sentry_example` in gcc/clang. This allows us to keep crash triggers simple.
		# The effects besides reproducible code-gen across compiler versions, will be negligible for build- and runtime.
		target_compile_options(sentry_example PRIVATE $<BUILD_INTERFACE:-O0>)
	endif()

	# set static runtime if enabled
	if(SENTRY_BUILD_RUNTIMESTATIC AND MSVC)
		set_property(TARGET sentry_example PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()

	if(DEFINED SENTRY_FOLDER)
		set_target_properties(sentry_example PROPERTIES FOLDER ${SENTRY_FOLDER})
	endif()

	if(CMAKE_GENERATOR STREQUAL "Xcode")
		set(SENTRY_FIXTURE_OUTPUT_DIR "$<TARGET_BUNDLE_DIR:sentry_example>/..")
	else()
		set(SENTRY_FIXTURE_OUTPUT_DIR "$<TARGET_FILE_DIR:sentry_example>")
	endif()

	add_custom_command(TARGET sentry_example POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/minidump.dmp" "${SENTRY_FIXTURE_OUTPUT_DIR}/minidump.dmp")

	add_custom_command(TARGET sentry_example POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/view-hierarchy.json" "${SENTRY_FIXTURE_OUTPUT_DIR}/view-hierarchy.json")
endif()

# Limit the exported symbols when sentry is built as a shared library to those with a "sentry_" prefix:
# - we do this at the end of the file as to not affect subdirectories reading target_link_libraries from the parent.
# - we do this as PRIVATE since our version script does not make sense in any other project that adds us.
#
# Used linker parameters:
# `--build-id`: To have a build-id in the ELF object.
# `--version-script`: version script either hides "foreign" symbols or defers them as unknown ("U") to system libraries.
# FIXME: cmake 3.13 introduced target_link_options (blocked by Android)
if(SENTRY_BUILD_SHARED_LIBS)
	target_link_libraries(sentry PRIVATE
			"$<$<OR:$<PLATFORM_ID:Linux>,$<PLATFORM_ID:Android>>:-Wl,--build-id=sha1,--version-script=${PROJECT_SOURCE_DIR}/src/exports.map>")

	# Support 16KB page sizes
	target_link_libraries(sentry PRIVATE
	    "$<$<PLATFORM_ID:Android>:-Wl,-z,max-page-size=16384>"
	)
endif()
