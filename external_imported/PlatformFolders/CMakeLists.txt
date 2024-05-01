# For target_compile_features
cmake_minimum_required(VERSION 3.1.0 FATAL_ERROR)

set(PLATFORMFOLDERS_MAIN_PROJECT OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(PLATFORMFOLDERS_MAIN_PROJECT ON)
endif()

project(platform_folders VERSION 4.2.0 LANGUAGES CXX)

# BUILD_SHARED_LIBS is off by default, the library will be static by default
option(PLATFORMFOLDERS_BUILD_SHARED_LIBS "Build platform_folders shared library" ${BUILD_SHARED_LIBS})
option(PLATFORMFOLDERS_BUILD_TESTING "Build platform_folders tests" ${PLATFORMFOLDERS_MAIN_PROJECT})
option(PLATFORMFOLDERS_ENABLE_INSTALL "Enable platform_folders INSTALL target" ${PLATFORMFOLDERS_MAIN_PROJECT})

set(PLATFORMFOLDERS_TYPE STATIC)
if(PLATFORMFOLDERS_BUILD_SHARED_LIBS)
	set(PLATFORMFOLDERS_TYPE SHARED)
endif()

add_library(platform_folders ${PLATFORMFOLDERS_TYPE}
	sago/platform_folders.cpp
)

set_target_properties(platform_folders PROPERTIES DEBUG_POSTFIX "${CMAKE_DEBUG_POSTFIX}")

# Creates an alias so that people building in-tree (instead of using find_package)...
# can still link against the same target
add_library(sago::platform_folders ALIAS platform_folders)

# Defines standardized defaults for install paths
include(GNUInstallDirs)
# Where to search for the header while building
target_include_directories(platform_folders PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sago>
	# Controls where #include <sago/platform_folders.h> starts to look from
	# So /usr/include/<sago/platform_folders.h>
	# or C:\Program Files\platform_folders\include\<sago/platform_folders.h>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Define the header as public for installation
set_target_properties(platform_folders PROPERTIES
	PUBLIC_HEADER "sago/platform_folders.h"
)

# cxx_std_11 requires v3.8
if(CMAKE_VERSION VERSION_LESS "3.8.0")
	# Use old method of forcing C++11
	set(CMAKE_CXX_STANDARD 11)
	set(CMAKE_CXX_STANDARD_REQUIRED true)
else()
	# Require (minimum) C++11 when using header
	# PRIVATE means only at compile time
	target_compile_features(platform_folders PUBLIC cxx_std_11)
endif()

# cxx_nullptr exists in v3.1
target_compile_features(platform_folders PRIVATE cxx_nullptr)

# Cmake's find_package search path is different based on the system
# See https://cmake.org/cmake/help/latest/command/find_package.html for the list
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	# You can pass these when calling Cmake, so don't override if the user does
	if(NOT _WIN32_WINNT AND NOT WINVER)
		target_compile_definitions(platform_folders PRIVATE
			_WIN32_WINNT=0x0601
			WINVER=0x0601
		)
	endif()
	# Controls where the exports, config, and configversion files install to
	set(_PROJECT_INSTALL_CMAKE_DIR "${CMAKE_INSTALL_PREFIX}/cmake")
else()
	# When calling find_package(<name>)
	# it looks for /usr/lib/cmake/<name>/<name>Config.cmake
	set(_PROJECT_INSTALL_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/platform_folders")
endif()

if(PLATFORMFOLDERS_ENABLE_INSTALL)
	# Gives "Make install" esque operations a location to install to...
	# and creates a .cmake file to be exported
	install(TARGETS platform_folders
		EXPORT "platform_foldersConfig"
		LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
		ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
		# Tells it where to put the header files
		PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sago"
	)

	# "The install(TARGETS) and install(EXPORT) commands work together to install a target and a file to help import it"
	# Installs a cmake file which external projects can import.
	install(EXPORT "platform_foldersConfig"
		NAMESPACE sago::
		DESTINATION "${_PROJECT_INSTALL_CMAKE_DIR}"
	)

	# "The export command is used to generate a file exporting targets from a project build tree"
	# Creates an import file for external projects which are aware of the build tree.
	# May be useful for cross-compiling
	export(TARGETS platform_folders
		FILE "platform_folders-exports.cmake"
	)

	# For the config and configversion macros
	include(CMakePackageConfigHelpers)

	# Creates the project's ConfigVersion.cmake file
	# This allows for find_package() to use a version in the call
	write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/platform_foldersConfigVersion.cmake"
		# This'll require versioning in the project() call
		VERSION ${CMAKE_PROJECT_VERSION}
		# Just assuming Semver is followed
		COMPATIBILITY SameMajorVersion
	)

	# Install the ConfigVersion file, which is located in the build dir
	install(FILES
			"${CMAKE_CURRENT_BINARY_DIR}/platform_foldersConfigVersion.cmake"
		DESTINATION "${_PROJECT_INSTALL_CMAKE_DIR}"
	)
endif()

if(PLATFORMFOLDERS_BUILD_TESTING)
	enable_testing()
	add_subdirectory(test)
	add_executable(platform_folders_sample platform_folders.cpp)
	target_link_libraries(platform_folders_sample PRIVATE platform_folders)
endif()
