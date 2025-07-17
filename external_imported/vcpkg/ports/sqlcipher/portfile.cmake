vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_fail_port_install( ON_TARGET "UWP" "OSX" "Linux")

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO sqlcipher/sqlcipher
    REF v4.5.0
    SHA512 d4c4fd97269721cf6e6a3195f67ef8b23bfb91d38336e7e049e7dc5c6ace25909f456463ba4202bc715a768da6885127755b036c291c7fb599de84a0a4c4bb7f
    HEAD_REF master
)

# Don't use vcpkg_build_nmake, because it doesn't handle nmake targets correctly.
find_program(NMAKE nmake REQUIRED)

# Find tclsh Executable needed for Amalgamation of SQLite
file(GLOB TCLSH_CMD
		${CURRENT_INSTALLED_DIR}/tools/tcl/bin/tclsh*${VCPKG_HOST_EXECUTABLE_SUFFIX}
)
file(TO_NATIVE_PATH "${TCLSH_CMD}" TCLSH_CMD)
file(TO_NATIVE_PATH "${SOURCE_PATH}" SOURCE_PATH_NAT)

# Determine TCL version (e.g. [path]tclsh90s.exe -> 90)
string(REGEX REPLACE ^.*tclsh "" TCLVERSION ${TCLSH_CMD})
string(REGEX REPLACE [A-Za-z]?${VCPKG_HOST_EXECUTABLE_SUFFIX}$ "" TCLVERSION ${TCLVERSION})

list(APPEND NMAKE_OPTIONS
		TCLSH_CMD="${TCLSH_CMD}"
		TCLVERSION=${TCLVERSION}
		ORIGINAL_SRC="${SOURCE_PATH_NAT}"
		EXT_FEATURE_FLAGS=-DSQLITE_TEMP_STORE=2\ -DSQLITE_HAS_CODEC
		LTLIBS=libcrypto.lib
        LTLIBPATHS=/LIBPATH:"${CURRENT_INSTALLED_DIR}/lib/"
)

set(ENV{INCLUDE} "${CURRENT_INSTALLED_DIR}/include;$ENV{INCLUDE}")

# Creating amalgamation files
message(STATUS "Pre-building ${TARGET_TRIPLET}")
vcpkg_execute_required_process(
	COMMAND ${NMAKE} -f Makefile.msc /A /NOLOGO clean tcl
	${NMAKE_OPTIONS}
	WORKING_DIRECTORY ${SOURCE_PATH}
	LOGNAME pre-build-${TARGET_TRIPLET}
)
message(STATUS "Pre-building ${TARGET_TRIPLET} done")

# The rest of the build process with the CMakeLists.txt is merely a copy of sqlite3

file(COPY ${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt DESTINATION ${SOURCE_PATH})

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
    geopoly WITH_GEOPOLY
    json1 WITH_JSON1
    INVERTED_FEATURES
    tool SQLITE3_SKIP_TOOLS
)

vcpkg_configure_cmake(
    SOURCE_PATH ${SOURCE_PATH}
    PREFER_NINJA
    OPTIONS ${FEATURE_OPTIONS}
    OPTIONS_DEBUG
        -DSQLITE3_SKIP_TOOLS=ON
)

vcpkg_install_cmake()
vcpkg_fixup_cmake_targets(CONFIG_PATH share/${PORT} TARGET_PATH share/${PORT})

file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug/share)

if(NOT SQLITE3_SKIP_TOOLS AND EXISTS ${CURRENT_PACKAGES_DIR}/tools/${PORT}/sqlcipher-bin${VCPKG_HOST_EXECUTABLE_SUFFIX})
    file(RENAME ${CURRENT_PACKAGES_DIR}/tools/${PORT}/sqlcipher-bin${VCPKG_HOST_EXECUTABLE_SUFFIX} ${CURRENT_PACKAGES_DIR}/tools/${PORT}/sqlcipher${VCPKG_HOST_EXECUTABLE_SUFFIX})
endif()

configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/sqlcipher-config.in.cmake
    ${CURRENT_PACKAGES_DIR}/share/${PORT}/sqlcipher-config.cmake
    @ONLY
)

file(INSTALL ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)

vcpkg_copy_pdbs()
vcpkg_copy_tool_dependencies(${CURRENT_PACKAGES_DIR}/tools/${PORT})
