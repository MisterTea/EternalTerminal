if (VCPKG_LIBRARY_LINKAGE STREQUAL dynamic AND VCPKG_CRT_LINKAGE STREQUAL static)
    message(STATUS "Warning: Dynamic library with static CRT is not supported. Building static library.")
    set(VCPKG_LIBRARY_LINKAGE static)
endif()

set(PYTHON_VERSION_MAJOR  3)
set(PYTHON_VERSION_MINOR  10)
set(PYTHON_VERSION_PATCH  7)
set(PYTHON_VERSION        ${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}.${PYTHON_VERSION_PATCH})

set(PATCHES
    0001-only-build-required-projects.patch
    0003-use-vcpkg-zlib.patch
    0004-devendor-external-dependencies.patch
    0005-dont-copy-vcruntime.patch
    0008-python.pc.patch
    0009-bz2d.patch
    0010-dont-skip-rpath.patch
    0012-force-disable-curses.patch
    0013-configure-no-libcrypt.patch  # https://github.com/python/cpython/pull/28881
)

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    list(APPEND PATCHES 0002-static-library.patch)
endif()

# Fix build failures with GCC for built-in modules (https://github.com/microsoft/vcpkg/issues/26573)
if(VCPKG_CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND PATCHES 0011-gcc-ldflags-fix.patch)
endif()

# Python 3.9 removed support for Windows 7. This patch re-adds support for Windows 7 and is therefore
# required to build this port on Windows 7 itself due to Python using itself in its own build system.
if("deprecated-win7-support" IN_LIST FEATURES)
    list(APPEND PATCHES 0006-restore-support-for-windows-7.patch)
    message(WARNING "Windows 7 support is deprecated and may be removed at any time.")
elseif(VCPKG_TARGET_IS_WINDOWS AND CMAKE_SYSTEM_VERSION EQUAL 6.1)
    message(FATAL_ERROR "python3 requires the feature deprecated-win7-support when building on Windows 7.")
endif()

if(VCPKG_TARGET_IS_WINDOWS OR VCPKG_TARGET_IS_UWP)
    string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" PYTHON_ALLOW_EXTENSIONS)
    # The Windows 11 SDK has a problem that causes it to error on the resource files, so we patch that.
    vcpkg_get_windows_sdk(WINSDK_VERSION)
    if("${WINSDK_VERSION}" VERSION_GREATER_EQUAL "10.0.22000")
        list(APPEND PATCHES "0007-workaround-windows-11-sdk-rc-compiler-error.patch")
    endif()
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO python/cpython
    REF v${PYTHON_VERSION}
    SHA512 88bf6efef632a7dad7306a59b7d5da159947d6675f0d264f1f33aa49a5703b4e4595011de52098eb839cc648994ae143f668507be7209f6bf3fe8ae0ec6a9125
    HEAD_REF master
    PATCHES ${PATCHES}
)

vcpkg_replace_string("${SOURCE_PATH}/Makefile.pre.in" "$(INSTALL) -d -m $(DIRMODE)" "$(MKDIR_P)")

function(make_python_pkgconfig)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "FILE;INSTALL_ROOT;EXEC_PREFIX;INCLUDEDIR;ABIFLAGS" "")

    set(prefix "${CURRENT_PACKAGES_DIR}")
    set(libdir [[${prefix}/lib]])
    set(exec_prefix ${arg_EXEC_PREFIX})
    set(includedir ${arg_INCLUDEDIR})
    set(VERSION "${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}")
    set(ABIFLAGS ${arg_ABIFLAGS})

    string(REPLACE "python" "python-${VERSION}" out_file ${arg_FILE})
    set(out_full_path "${arg_INSTALL_ROOT}/lib/pkgconfig/${out_file}")
    configure_file("${SOURCE_PATH}/Misc/${arg_FILE}.in" ${out_full_path} @ONLY)

    file(READ ${out_full_path} pkgconfig_file)
    string(REPLACE "-lpython${VERSION}" "-lpython${PYTHON_VERSION_MAJOR}${PYTHON_VERSION_MINOR}" pkgconfig_file "${pkgconfig_file}")
    file(WRITE ${out_full_path} "${pkgconfig_file}")
endfunction()

if(VCPKG_TARGET_IS_WINDOWS OR VCPKG_TARGET_IS_UWP)
    # Due to the way Python handles C extension modules on Windows, a static python core cannot
    # load extension modules.
    if(PYTHON_ALLOW_EXTENSIONS)
        find_library(BZ2_RELEASE NAMES bz2 PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
        find_library(BZ2_DEBUG NAMES bz2d PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)
        find_library(CRYPTO_RELEASE NAMES libcrypto PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
        find_library(CRYPTO_DEBUG NAMES libcrypto PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)
        find_library(EXPAT_RELEASE NAMES libexpat libexpatMD libexpatMT PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
        find_library(EXPAT_DEBUG NAMES libexpatd libexpatdMD libexpatdMT PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)
        find_library(FFI_RELEASE NAMES libffi PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
        find_library(FFI_DEBUG NAMES libffi PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)
        find_library(LZMA_RELEASE NAMES lzma PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
        find_library(LZMA_DEBUG NAMES lzma PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)
        find_library(SQLITE_RELEASE NAMES sqlite3 PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
        find_library(SQLITE_DEBUG NAMES sqlite3 PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)
        find_library(SSL_RELEASE NAMES libssl PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
        find_library(SSL_DEBUG NAMES libssl PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)
    else()
        message(STATUS "WARNING: Static builds of Python will not have C extension modules available.")
    endif()
    find_library(ZLIB_RELEASE NAMES zlib PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
    find_library(ZLIB_DEBUG NAMES zlib zlibd PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)

    configure_file("${SOURCE_PATH}/PC/pyconfig.h" "${SOURCE_PATH}/PC/pyconfig.h")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/python_vcpkg.props.in" "${SOURCE_PATH}/PCbuild/python_vcpkg.props")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/openssl.props.in" "${SOURCE_PATH}/PCbuild/openssl.props")
    file(WRITE "${SOURCE_PATH}/PCbuild/libffi.props"
        "<?xml version='1.0' encoding='utf-8'?>
        <Project xmlns='http://schemas.microsoft.com/developer/msbuild/2003' />"
    )

    if(PYTHON_ALLOW_EXTENSIONS)
        set(OPTIONS
            "/p:IncludeExtensions=true"
            "/p:IncludeExternals=true"
            "/p:IncludeCTypes=true"
            "/p:IncludeSSL=true"
            "/p:IncludeTkinter=false"
            "/p:IncludeTests=false"
            "/p:ForceImportBeforeCppTargets=${SOURCE_PATH}/PCbuild/python_vcpkg.props"
        )
    else()
        set(OPTIONS
            "/p:IncludeExtensions=false"
            "/p:IncludeExternals=false"
            "/p:IncludeTests=false"
            "/p:ForceImportBeforeCppTargets=${SOURCE_PATH}/PCbuild/python_vcpkg.props"
        )
    endif()
    if(VCPKG_TARGET_IS_UWP)
        list(APPEND OPTIONS "/p:IncludeUwp=true")
    else()
        list(APPEND OPTIONS "/p:IncludeUwp=false")
    endif()
    if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
        list(APPEND OPTIONS "/p:_VcpkgPythonLinkage=DynamicLibrary")
    else()
        list(APPEND OPTIONS "/p:_VcpkgPythonLinkage=StaticLibrary")
    endif()

    # _freeze_importlib.exe is run as part of the build process, so make sure the required dynamic libs are available.
    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "release")
        vcpkg_add_to_path("${CURRENT_INSTALLED_DIR}/bin")
    endif()
    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
        vcpkg_add_to_path("${CURRENT_INSTALLED_DIR}/debug/bin")
    endif()

    vcpkg_install_msbuild(
        SOURCE_PATH "${SOURCE_PATH}"
        PROJECT_SUBPATH "PCbuild/pcbuild.proj"
        OPTIONS ${OPTIONS}
        LICENSE_SUBPATH "LICENSE"
        TARGET_PLATFORM_VERSION "${WINSDK_VERSION}"
        SKIP_CLEAN
    )

    # The extension modules must be placed in the DLLs directory, so we can't use vcpkg_copy_tools()
    if(PYTHON_ALLOW_EXTENSIONS)
        file(GLOB_RECURSE PYTHON_EXTENSIONS "${CURRENT_BUILDTREES_DIR}/*.pyd")
        list(FILTER PYTHON_EXTENSIONS EXCLUDE REGEX [[.*_d\.pyd]])
        file(COPY ${PYTHON_EXTENSIONS} DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}/DLLs")
        vcpkg_copy_tool_dependencies("${CURRENT_PACKAGES_DIR}/tools/${PORT}/DLLs")
        file(REMOVE "${CURRENT_PACKAGES_DIR}/tools/${PORT}/DLLs/python${PYTHON_VERSION_MAJOR}${PYTHON_VERSION_MINOR}.dll")
    endif()

    file(COPY "${SOURCE_PATH}/Include/" "${SOURCE_PATH}/PC/pyconfig.h"
        DESTINATION "${CURRENT_PACKAGES_DIR}/include/python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}"
        FILES_MATCHING PATTERN *.h
    )
    file(COPY "${SOURCE_PATH}/Lib" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}")

    # Remove any extension libraries and other unversioned binaries that could conflict with the python2 port.
    # You don't need to link against these anyway.
    file(GLOB PYTHON_LIBS
        "${CURRENT_PACKAGES_DIR}/lib/*.lib"
        "${CURRENT_PACKAGES_DIR}/debug/lib/*.lib"
    )
    list(FILTER PYTHON_LIBS EXCLUDE REGEX [[python[0-9]*(_d)?\.lib$]])
    file(GLOB PYTHON_INSTALLERS "${CURRENT_PACKAGES_DIR}/tools/${PORT}/wininst-*.exe")
    file(REMOVE ${PYTHON_LIBS} ${PYTHON_INSTALLERS})

    # pkg-config files
    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "release")
        make_python_pkgconfig(FILE python.pc INSTALL_ROOT ${CURRENT_PACKAGES_DIR}
            EXEC_PREFIX "\${prefix}/tools/${PORT}" INCLUDEDIR [[${prefix}/include]] ABIFLAGS "")
        make_python_pkgconfig(FILE python-embed.pc INSTALL_ROOT ${CURRENT_PACKAGES_DIR}
            EXEC_PREFIX "\${prefix}/tools/${PORT}" INCLUDEDIR [[${prefix}/include]] ABIFLAGS "")
    endif()

    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
        make_python_pkgconfig(FILE python.pc INSTALL_ROOT "${CURRENT_PACKAGES_DIR}/debug"
            EXEC_PREFIX "\${prefix}/../tools/${PORT}" INCLUDEDIR [[${prefix}/../include]] ABIFLAGS "_d")
        make_python_pkgconfig(FILE python-embed.pc INSTALL_ROOT "${CURRENT_PACKAGES_DIR}/debug"
            EXEC_PREFIX "\${prefix}/../tools/${PORT}" INCLUDEDIR [[${prefix}/../include]] ABIFLAGS "_d")
    endif()

    vcpkg_fixup_pkgconfig()

    vcpkg_clean_msbuild()

    # Remove static library belonging to executable
    if (VCPKG_LIBRARY_LINKAGE STREQUAL "static")
        if (EXISTS "${CURRENT_PACKAGES_DIR}/lib/python.lib")
            file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/lib/manual-link")
            file(RENAME "${CURRENT_PACKAGES_DIR}/lib/python.lib"
                "${CURRENT_PACKAGES_DIR}/lib/manual-link/python.lib")
        endif()
        if (EXISTS "${CURRENT_PACKAGES_DIR}/debug/lib/python_d.lib")
            file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/debug/lib/manual-link")
            file(RENAME "${CURRENT_PACKAGES_DIR}/debug/lib/python_d.lib"
                "${CURRENT_PACKAGES_DIR}/debug/lib/manual-link/python_d.lib")
        endif()
    endif()
else()
    # The Python Stable ABI, `libpython3.so` is not produced by the upstream build system with --with-pydebug option
    if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic" AND NOT VCPKG_BUILD_TYPE)
        set(VCPKG_POLICY_MISMATCHED_NUMBER_OF_BINARIES enabled)
    endif()

    set(OPTIONS
        "--with-openssl=${CURRENT_INSTALLED_DIR}"
        "--without-ensurepip"
        "--with-suffix="
        "--with-system-expat"
        "--without-readline"
        "--disable-test-modules"
    )
    if(VCPKG_TARGET_IS_OSX)
        list(APPEND OPTIONS "LIBS=-liconv -lintl")
    endif()

    vcpkg_configure_make(
        SOURCE_PATH "${SOURCE_PATH}"
        OPTIONS ${OPTIONS}
        OPTIONS_DEBUG "--with-pydebug"
    )
    vcpkg_install_make(ADD_BIN_TO_PATH INSTALL_TARGET altinstall)

    file(COPY "${CURRENT_PACKAGES_DIR}/tools/${PORT}/bin/" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}")

    # Makefiles, c files, __pycache__, and other junk.
    file(GLOB PYTHON_LIB_DIRS LIST_DIRECTORIES true
        "${CURRENT_PACKAGES_DIR}/lib/python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}/*"
        "${CURRENT_PACKAGES_DIR}/debug/lib/python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}/*")
    list(FILTER PYTHON_LIB_DIRS INCLUDE REGEX [[config-[0-9].*.*]])
    file(REMOVE_RECURSE ${PYTHON_LIB_DIRS})

    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/bin")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}d")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/${PORT}/man1")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/tools/${PORT}/bin")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/tools/${PORT}/debug")

    file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME "copyright")

    vcpkg_fixup_pkgconfig()

    # Perform some post-build checks on modules
    file(GLOB python_libs_dynload_debug LIST_DIRECTORIES false "${CURRENT_PACKAGES_DIR}/debug/lib/python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}/lib-dynload/*.so*")
    file(GLOB python_libs_dynload_release LIST_DIRECTORIES false "${CURRENT_PACKAGES_DIR}/lib/python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}/lib-dynload/*.so*")
    set(python_libs_dynload_failed_debug ${python_libs_dynload_debug})
    set(python_libs_dynload_failed_release ${python_libs_dynload_release})
    list(FILTER python_libs_dynload_failed_debug INCLUDE REGEX ".*_failed\.so.*")
    list(FILTER python_libs_dynload_failed_release INCLUDE REGEX ".*_failed\.so.*")
    if(python_libs_dynload_failed_debug OR python_libs_dynload_failed_release)
        list(JOIN python_libs_dynload_failed_debug "\n" python_libs_dynload_failed_debug_str)
        list(JOIN python_libs_dynload_failed_release "\n" python_libs_dynload_failed_release_str)
        message(FATAL_ERROR "There should be no modules with \"_failed\" suffix:\n${python_libs_dynload_failed_debug_str}\n${python_libs_dynload_failed_release_str}")
    endif()
    if(NOT VCPKG_BUILD_TYPE)
        list(LENGTH python_libs_dynload_release python_libs_dynload_release_length)
        list(LENGTH python_libs_dynload_debug python_libs_dynload_debug_length)
        if(NOT python_libs_dynload_release_length STREQUAL python_libs_dynload_debug_length)
            message(FATAL_ERROR "Mismatched number of modules: ${python_libs_dynload_debug_length} in debug, ${python_libs_dynload_release_length} in release")
        endif()
    endif()
endif()

file(READ "${CMAKE_CURRENT_LIST_DIR}/usage" usage)
if(VCPKG_TARGET_IS_WINDOWS)
    if(PYTHON_ALLOW_EXTENSIONS)
        file(READ "${CMAKE_CURRENT_LIST_DIR}/usage.win" usage_extra)
    else()
        set(usage_extra "")
    endif()
else()
    file(READ "${CMAKE_CURRENT_LIST_DIR}/usage.unix" usage_extra)
endif()
file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" "${usage}\n${usage_extra}")

function(_generate_finder)
    cmake_parse_arguments(PythonFinder "NO_OVERRIDE" "DIRECTORY;PREFIX" "" ${ARGN})
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/vcpkg-cmake-wrapper.cmake"
        "${CURRENT_PACKAGES_DIR}/share/${PythonFinder_DIRECTORY}/vcpkg-cmake-wrapper.cmake"
        @ONLY
    )
endfunction()

message(STATUS "Installing cmake wrappers")
_generate_finder(DIRECTORY "python" PREFIX "Python")
_generate_finder(DIRECTORY "python3" PREFIX "Python3")
_generate_finder(DIRECTORY "pythoninterp" PREFIX "PYTHON" NO_OVERRIDE)

if (NOT VCPKG_TARGET_IS_WINDOWS)
    function(replace_dirs_in_config_file python_config_file)
        vcpkg_replace_string("${python_config_file}" "${CURRENT_INSTALLED_DIR}" "' + _base + '")
        vcpkg_replace_string("${python_config_file}" "${CURRENT_PACKAGES_DIR}" "' + _base + '")
        vcpkg_replace_string("${python_config_file}" "${CURRENT_BUILDTREES_DIR}" "not/existing")
    endfunction()

    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "release")
        file(GLOB python_config_files "${CURRENT_PACKAGES_DIR}/lib/python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}/_sysconfigdata*")
        list(POP_FRONT python_config_files python_config_file)
        vcpkg_replace_string("${python_config_file}" "# system configuration generated and used by the sysconfig module" "# system configuration generated and used by the sysconfig module\nimport os\n_base = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))\n")
        replace_dirs_in_config_file("${python_config_file}")
    endif()

    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
        file(GLOB python_config_files "${CURRENT_PACKAGES_DIR}/debug/lib/python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}/_sysconfigdata*")
        list(POP_FRONT python_config_files python_config_file)
        vcpkg_replace_string("${python_config_file}" "# system configuration generated and used by the sysconfig module" "# system configuration generated and used by the sysconfig module\nimport os\n_base = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(__file__))))\n")
        replace_dirs_in_config_file("${python_config_file}")
    endif()
endif()
